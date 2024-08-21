// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txgraph.h>

#include <cluster_linearize.h>
#include <random.h>
#include <util/bitset.h>
#include <util/check.h>
#include <util/feefrac.h>

#include <compare>
#include <memory>
#include <span>
#include <utility>

namespace {

using namespace cluster_linearize;

// Forward declare the TxGraph implementation class.
class TxGraphImpl;

/** Position of a DepGraphIndex within a Cluster::m_linearization. */
using LinearizationIndex = uint32_t;
/** Position of a Cluster within Graph::m_clusters. */
using ClusterSetIndex = uint32_t;

/** Quality levels for cached cluster linearizations. */
enum class QualityLevel
{
    /** This cluster may have multiple disconnected components, which are all NEEDS_RELINEARIZE. */
    NEEDS_SPLIT,
    /** This cluster has undergone changes that warrant re-linearization. */
    NEEDS_RELINEARIZE,
    /** The minimal level of linearization has been performed, but it is not known to be optimal. */
    ACCEPTABLE,
    /** The linearization is known to be optimal. */
    OPTIMAL,
    /** This cluster is not registered in any m_clusters.
     *  This must be the last entry in QualityLevel as m_clusters is sized using it. */
    NONE,
};

/** A grouping of connected transactions inside a TxGraphImpl. */
class Cluster
{
    friend class TxGraphImpl;
    using GraphIndex = TxGraph::GraphIndex;
    using SetType = BitSet<CLUSTER_COUNT_LIMIT>;
    /** The DepGraph for this cluster, holding all feerates, and ancestors/descendants. */
    DepGraph<SetType> m_depgraph;
    /** m_mapping[i] gives the GraphIndex for the position i transaction in m_depgraph. Values for
     *  positions i that do not exist in m_depgraph shouldn't ever be accessed and thus don't
     *  matter. m_mapping.size() equals m_depgraph.PositionRange(). */
    std::vector<GraphIndex> m_mapping;
    /** The current linearization of the cluster. m_linearization.size() equals
     *  m_depgraph.TxCount(). This is always kept topological. */
    std::vector<DepGraphIndex> m_linearization;
    /** The quality level of m_linearization. */
    QualityLevel m_quality{QualityLevel::NONE};
    /** Which position this Cluster has in Graph::m_clusters[m_quality]. */
    ClusterSetIndex m_setindex{ClusterSetIndex(-1)};

public:
    /** Construct an empty Cluster. */
    Cluster() noexcept = default;
    /** Construct a singleton Cluster. */
    explicit Cluster(TxGraphImpl& graph, const FeePerWeight& feerate, GraphIndex graph_index) noexcept;

    // Cannot move or copy (would invalidate Cluster* in Locator and TxGraphImpl). */
    Cluster(const Cluster&) = delete;
    Cluster& operator=(const Cluster&) = delete;
    Cluster(Cluster&&) = delete;
    Cluster& operator=(Cluster&&) = delete;

    // Generic helper functions.

    /** Whether the linearization of this Cluster can be exposed. */
    bool IsAcceptable() const noexcept
    {
        return m_quality == QualityLevel::ACCEPTABLE || m_quality == QualityLevel::OPTIMAL;
    }
    /** Whether the linearization of this Cluster is optimal. */
    bool IsOptimal() const noexcept
    {
        return m_quality == QualityLevel::OPTIMAL;
    }
    /** Whether this cluster requires splitting. */
    bool NeedsSplitting() const noexcept
    {
        return m_quality == QualityLevel::NEEDS_SPLIT;
    }
    /** Get the number of transactions in this Cluster. */
    LinearizationIndex GetTxCount() const noexcept { return m_linearization.size(); }
    /** Only called by Graph::SwapIndexes. */
    void UpdateMapping(DepGraphIndex cluster_idx, GraphIndex graph_idx) noexcept { m_mapping[cluster_idx] = graph_idx; }
    /** Push changes to Cluster and its linearization to the TxGraphImpl Entry objects. */
    void Updated(TxGraphImpl& graph) noexcept;

    // Functions that implement the Cluster-specific side of internal TxGraphImpl mutations.

    /** Apply all removals from the front of to_remove that apply to this Cluster, popping them
     *  off. These must be at least one such entry. */
    void ApplyRemovals(TxGraphImpl& graph, std::span<GraphIndex>& to_remove) noexcept;
    /** Split this cluster (must have a NEEDS_SPLIT* quality). Returns whether to delete this
     *  Cluster afterwards. */
    [[nodiscard]] bool Split(TxGraphImpl& graph) noexcept;
    /** Move all transactions from cluster to *this (as separate components). */
    void Merge(TxGraphImpl& graph, Cluster& cluster) noexcept;
    /** Given a span of (parent, child) pairs that all belong to this Cluster, apply them. */
    void ApplyDependencies(TxGraphImpl& graph, std::span<std::pair<GraphIndex, GraphIndex>> to_apply) noexcept;
    /** Improve the linearization of this Cluster. */
    void Relinearize(TxGraphImpl& graph, uint64_t max_iters) noexcept;

    // Functions that implement the Cluster-specific side of public TxGraph functions.

    /** Get a vector of Refs for the ancestors of a given Cluster element. */
    std::vector<TxGraph::Ref*> GetAncestorRefs(const TxGraphImpl& graph, DepGraphIndex idx) noexcept;
    /** Get a vector of Refs for the descendants of a given Cluster element. */
    std::vector<TxGraph::Ref*> GetDescendantRefs(const TxGraphImpl& graph, DepGraphIndex idx) noexcept;
    /** Get a vector of Refs for all elements of this Cluster, in linearization order. */
    std::vector<TxGraph::Ref*> GetClusterRefs(const TxGraphImpl& graph) noexcept;
    /** Get the individual transaction feerate of a Cluster element. */
    FeePerWeight GetIndividualFeerate(DepGraphIndex idx) noexcept;
    /** Modify the fee of a Cluster element. */
    void SetFee(TxGraphImpl& graph, DepGraphIndex idx, int64_t fee) noexcept;
};

/** The transaction graph.
 *
 * The overall design of the data structure consists of 3 interlinked representations:
 * - The transactions (held as a vector of TxGraphImpl::Entry inside TxGraphImpl).
 * - The clusters (Cluster objects in per-quality vectors inside TxGraphImpl).
 * - The Refs (TxGraph::Ref objects, held externally by users of the TxGraph class)
 *
 * Clusters and Refs contain the index of the Entry objects they refer to, and the Entry objects
 * refer back to the Clusters and Refs the corresponding transaction is contained in.
 *
 * While redundant, this permits moving all of them independently, without invalidating things
 * or costly iteration to fix up everything:
 * - Entry objects can be moved to fill holes left by removed transactions in the Entry vector
 *   (see TxGraphImpl::Compact).
 * - Clusters can be rewritten continuously (removals can cause them to split, new dependencies
 *   can cause them to be merged).
 * - Ref objects can be held outside the class, while permitting them to be moved around, and
 *   inherited from.
 */
class TxGraphImpl final : public TxGraph
{
    friend class Cluster;
private:
    /** Internal RNG. */
    FastRandomContext m_rng;

    /** Information about one group of Clusters to be merged. */
    struct GroupEntry
    {
        /** Which clusters are to be merged. */
        std::vector<Cluster*> m_clusters;
        /** Which dependencies are to be applied to those merged clusters, as (parent, child)
         *  pairs. */
        std::vector<std::pair<GraphIndex, GraphIndex>> m_deps;
    };

    /** The vectors of clusters, one vector per quality level. ClusterSetIndex indexes into each. */
    std::array<std::vector<std::unique_ptr<Cluster>>, int(QualityLevel::NONE)> m_clusters;
    /** Which removals have yet to be applied. */
    std::vector<GraphIndex> m_to_remove;
    /** Which dependencies are to be added ((parent,child) pairs). GroupData::m_deps_offset indexes
     *  into this. */
    std::vector<std::pair<GraphIndex, GraphIndex>> m_deps_to_add;
    /** Information about the merges to be performed, if known. */
    std::optional<std::vector<GroupEntry>> m_group_data = std::vector<GroupEntry>{};
    /** Total number of transactions in this graph (sum of all transaction counts in all Clusters).
     *  */
    GraphIndex m_txcount{0};

    /** A Locator that describes whether, where, and in which Cluster an Entry appears. */
    struct Locator
    {
        /** Which Cluster the Entry appears in (nullptr = missing). */
        Cluster* cluster{nullptr};
        /** Where in the Cluster it appears (only if cluster != nullptr). */
        DepGraphIndex index{0};

        /** Mark this Locator as missing. */
        void SetMissing() noexcept { cluster = nullptr; index = 0; }
        /** Mark this Locator as present, in the specified Cluster. */
        void SetPresent(Cluster* c, DepGraphIndex i) noexcept { cluster = c; index = i; }
        /** Check if this Locator is present (in some Cluster). */
        bool IsPresent() const noexcept { return cluster != nullptr; }
    };

    /** Internal information about each transaction in a TxGraphImpl. */
    struct Entry
    {
        /** Pointer to the corresponding Ref object if any, or nullptr if unlinked. */
        Ref* m_ref{nullptr};
        /** Which Cluster and position therein this Entry appears in. */
        Locator m_locator;
    };

    /** The set of all transactions. GraphIndex values index into this. */
    std::vector<Entry> m_entries;

    /** Set of Entries which have no linked Ref anymore. */
    std::vector<GraphIndex> m_unlinked;

public:
    /** Construct a new TxGraphImpl. */
    explicit TxGraphImpl() noexcept {}

    // Cannot move or copy (would invalidate TxGraphImpl* in Ref, MiningOrder, EvictionOrder).
    TxGraphImpl(const TxGraphImpl&) = delete;
    TxGraphImpl& operator=(const TxGraphImpl&) = delete;
    TxGraphImpl(TxGraphImpl&&) = delete;
    TxGraphImpl& operator=(TxGraphImpl&&) = delete;

    // Simple helper functions.

    /** Swap the Entrys referred to by a and b. */
    void SwapIndexes(GraphIndex a, GraphIndex b) noexcept;
    /** Extract a Cluster. */
    std::unique_ptr<Cluster> ExtractCluster(QualityLevel quality, ClusterSetIndex setindex) noexcept;
    /** Delete a Cluster. */
    void DeleteCluster(Cluster& cluster) noexcept;
    /** Insert a Cluster. */
    ClusterSetIndex InsertCluster(std::unique_ptr<Cluster>&& cluster, QualityLevel quality) noexcept;
    /** Change the QualityLevel of a Cluster (identified by old_quality and old_index). */
    void SetClusterQuality(QualityLevel old_quality, ClusterSetIndex old_index, QualityLevel new_quality) noexcept;

    // Functions for handling Refs.

    /** Only called by Ref's move constructor/assignment to update Ref locations. */
    void UpdateRef(GraphIndex idx, Ref& new_location) noexcept final
    {
        auto& entry = m_entries[idx];
        Assume(entry.m_ref != nullptr);
        entry.m_ref = &new_location;
    }

    /** Only called by Ref::~Ref to unlink Refs, and Ref's move assignment. */
    void UnlinkRef(GraphIndex idx) noexcept final
    {
        auto& entry = m_entries[idx];
        Assume(entry.m_ref != nullptr);
        entry.m_ref = nullptr;
        m_unlinked.push_back(idx);
        Compact();
    }

    // Functions related to various normalization/application steps.
    /** Get rid of unlinked Entry objects in m_entries, if possible (this changes the GraphIndex
     *  values for remaining Entrys, so this only does something when no to-be-applied operations
     *  referring to GraphIndexes remain). */
    void Compact() noexcept;
    /** Apply all removals queued up in m_to_remove to the relevant Clusters (which get a
     *  NEEDS_SPLIT* QualityLevel). */
    void ApplyRemovals() noexcept;
    /** Split an individual cluster. */
    void Split(Cluster& cluster) noexcept;
    /** Split all clusters that need splitting. */
    void SplitAll() noexcept;
    /** Populate m_group_data based on m_deps_to_add. */
    void GroupClusters() noexcept;
    /** Merge the specified clusters. */
    void Merge(std::span<Cluster*> to_merge) noexcept;
    /** Apply all m_deps_to_add to the relevant Clusters. */
    void ApplyDependencies() noexcept;
    /** Make a specified Cluster have quality ACCEPTABLE or OPTIMAL. */
    void MakeAcceptable(Cluster& cluster) noexcept;

    // Implementations for the public TxGraph interface.

    Ref AddTransaction(const FeePerWeight& feerate) noexcept final;
    void RemoveTransaction(const Ref& arg) noexcept final;
    void AddDependency(const Ref& parent, const Ref& child) noexcept final;
    void SetTransactionFee(const Ref&, int64_t fee) noexcept final;

    bool Exists(const Ref& arg) noexcept final;
    FeePerWeight GetIndividualFeerate(const Ref& arg) noexcept final;
    std::vector<Ref*> GetCluster(const Ref& arg) noexcept final;
    std::vector<Ref*> GetAncestors(const Ref& arg) noexcept final;
    std::vector<Ref*> GetDescendants(const Ref& arg) noexcept final;
    GraphIndex GetTransactionCount() noexcept final;
};

void Cluster::Updated(TxGraphImpl& graph) noexcept
{
    // Update all the Locators for this Cluster's Entrys.
    for (DepGraphIndex idx : m_linearization) {
        auto& entry = graph.m_entries[m_mapping[idx]];
        entry.m_locator.SetPresent(this, idx);
    }
}

void Cluster::ApplyRemovals(TxGraphImpl& graph, std::span<GraphIndex>& to_remove) noexcept
{
    // Iterate over the prefix of to_remove that applies to this cluster.
    Assume(!to_remove.empty());
    SetType todo;
    do {
        GraphIndex idx = to_remove.front();
        Assume(idx < graph.m_entries.size());
        auto& entry = graph.m_entries[idx];
        auto& locator = entry.m_locator;
        // Stop once we hit an entry that applies to another Cluster.
        if (locator.cluster != this) break;
        // - Remember it in a set of to-remove DepGraphIndexes.
        todo.Set(locator.index);
        // - Remove from m_mapping. This isn't strictly necessary as unused positions in m_mapping
        //   are just never accessed, but set it to -1 here to increase the ability to detect a bug
        //   that causes it to be accessed regardless.
        m_mapping[locator.index] = GraphIndex(-1);
        // - Mark it as removed in the Entry's locator.
        locator.SetMissing();
        to_remove = to_remove.subspan(1);
        --graph.m_txcount;
    } while(!to_remove.empty());

    Assume(todo.Any());
    // Wipe from the Cluster's DepGraph (this is O(n) regardless of the number of entries
    // removed, so we benefit from batching all the removals).
    m_depgraph.RemoveTransactions(todo);
    m_mapping.resize(m_depgraph.PositionRange());

    // Filter removals out of m_linearization.
    m_linearization.erase(std::remove_if(
        m_linearization.begin(),
        m_linearization.end(),
        [&](auto pos) { return todo[pos]; }), m_linearization.end());

    graph.SetClusterQuality(m_quality, m_setindex, QualityLevel::NEEDS_SPLIT);
    Updated(graph);
}

bool Cluster::Split(TxGraphImpl& graph) noexcept
{
    // This function can only be called when the Cluster needs splitting.
    Assume(NeedsSplitting());
    /** Which positions are still left in this Cluster. */
    auto todo = m_depgraph.Positions();
    /** Mapping from transaction positions in this Cluster to the Cluster where it ends up, and
     *  its position therein. */
    std::vector<std::pair<Cluster*, DepGraphIndex>> remap(m_depgraph.PositionRange());
    std::vector<Cluster*> new_clusters;
    bool first{true};
    // Iterate over the connected components of this Cluster's m_depgraph.
    while (todo.Any()) {
        auto component = m_depgraph.FindConnectedComponent(todo);
        if (first && component == todo) {
            // The existing Cluster is an entire component. Leave it be, but update its quality.
            Assume(todo == m_depgraph.Positions());
            graph.SetClusterQuality(m_quality, m_setindex, QualityLevel::NEEDS_RELINEARIZE);
            // We need to recompute and cache its chunking.
            Updated(graph);
            return false;
        }
        first = false;
        // Construct a new Cluster to hold the found component.
        auto new_cluster = std::make_unique<Cluster>();
        new_clusters.push_back(new_cluster.get());
        // Remember that all the component's transactions go to this new Cluster. The positions
        // will be determined below, so use -1 for now.
        for (auto i : component) {
            remap[i] = {new_cluster.get(), DepGraphIndex(-1)};
        }
        graph.InsertCluster(std::move(new_cluster), QualityLevel::NEEDS_RELINEARIZE);
        todo -= component;
    }
    // Redistribute the transactions.
    for (auto i : m_linearization) {
        /** The cluster which transaction originally in position i is moved to. */
        Cluster* new_cluster = remap[i].first;
        // Copy the transaction to the new cluster's depgraph, and remember the position.
        remap[i].second = new_cluster->m_depgraph.AddTransaction(m_depgraph.FeeRate(i));
        // Create new mapping entry.
        new_cluster->m_mapping.push_back(m_mapping[i]);
        // Create a new linearization entry. As we're only appending transactions, they equal the
        // DepGraphIndex.
        new_cluster->m_linearization.push_back(remap[i].second);
    }
    // Redistribute the dependencies.
    for (auto i : m_linearization) {
        /** The cluster transaction in position i is moved to. */
        Cluster* new_cluster = remap[i].first;
        // Copy its parents, translating positions.
        SetType new_parents;
        for (auto par : m_depgraph.GetReducedParents(i)) new_parents.Set(remap[par].second);
        new_cluster->m_depgraph.AddDependencies(new_parents, remap[i].second);
    }
    // Update all the Locators of moved transactions.
    for (Cluster* new_cluster : new_clusters) {
        new_cluster->Updated(graph);
    }
    // Wipe this Cluster, and return that it needs to be deleted.
    m_depgraph = DepGraph<SetType>{};
    m_mapping.clear();
    m_linearization.clear();
    return true;
}

void Cluster::Merge(TxGraphImpl& graph, Cluster& other) noexcept
{
    /** Vector to store the positions in this Cluster for each position in other. */
    std::vector<DepGraphIndex> remap(other.m_depgraph.PositionRange());
    // Iterate over all transactions in the other Cluster (the one being absorbed).
    for (auto pos : other.m_linearization) {
        auto idx = other.m_mapping[pos];
        // Copy the transaction into this Cluster, and remember its position.
        auto new_pos = m_depgraph.AddTransaction(other.m_depgraph.FeeRate(pos));
        remap[pos] = new_pos;
        if (new_pos == m_mapping.size()) {
            m_mapping.push_back(idx);
        } else {
            m_mapping[new_pos] = idx;
        }
        m_linearization.push_back(new_pos);
        // Copy the transaction's dependencies, translating them using remap. Note that since
        // pos iterates over other.m_linearization, which is in topological order, all parents
        // of pos should already be in remap.
        SetType parents;
        for (auto par : other.m_depgraph.GetReducedParents(pos)) {
            parents.Set(remap[par]);
        }
        m_depgraph.AddDependencies(parents, remap[pos]);
        // Update the transaction's Locator. There is no need to call Updated() to update chunk
        // feerates, as Updated() will be invoked by Cluster::ApplyDependencies on the resulting
        // merged Cluster later anyway).
        graph.m_entries[idx].m_locator.SetPresent(this, new_pos);
    }
    // Purge the other Cluster, now that everything has been moved.
    other.m_depgraph = DepGraph<SetType>{};
    other.m_linearization.clear();
    other.m_mapping.clear();
}

void Cluster::ApplyDependencies(TxGraphImpl& graph, std::span<std::pair<GraphIndex, GraphIndex>> to_apply) noexcept
{
    // This function is invoked by TxGraphImpl::ApplyDependencies after merging groups of Clusters
    // between which dependencies are added, which simply concatenates their linearizations. Invoke
    // PostLinearize, which has the effect that the linearization becomes a merge-sort of the
    // constituent linearizations. Do this here rather than in Cluster::Merge, because this
    // function is only invoked once per merged Cluster, rather than once per constituent one.
    // This concatenation + post-linearization could be replaced with an explicit merge-sort.
    PostLinearize(m_depgraph, m_linearization);

    // Sort the list of dependencies to apply by child, so those can be applied in batch.
    std::sort(to_apply.begin(), to_apply.end(), [](auto& a, auto& b) { return a.second < b.second; });
    // Iterate over groups of to-be-added dependencies with the same child.
    auto it = to_apply.begin();
    while (it != to_apply.end()) {
        auto& first_child = graph.m_entries[it->second].m_locator;
        const auto child_idx = first_child.index;
        // Iterate over all to-be-added dependencies within that same child, gather the relevant
        // parents.
        SetType parents;
        while (it != to_apply.end()) {
            auto& child = graph.m_entries[it->second].m_locator;
            auto& parent = graph.m_entries[it->first].m_locator;
            Assume(child.cluster == this && parent.cluster == this);
            if (child.index != child_idx) break;
            parents.Set(parent.index);
            ++it;
        }
        // Push all dependencies to the underlying DepGraph. Note that this is O(N) in the size of
        // the cluster, regardless of the number of parents being added, so batching them together
        // has a performance benefit.
        m_depgraph.AddDependencies(parents, child_idx);
    }

    // Finally fix the linearization, as the new dependencies may have invalidated the
    // linearization, and post-linearize it to fix up the worst problems with it.
    FixLinearization(m_depgraph, m_linearization);
    PostLinearize(m_depgraph, m_linearization);

    // Finally push the changes to graph.m_entries.
    Updated(graph);
}

std::unique_ptr<Cluster> TxGraphImpl::ExtractCluster(QualityLevel quality, ClusterSetIndex setindex) noexcept
{
    Assume(quality != QualityLevel::NONE);

    auto& quality_clusters = m_clusters[int(quality)];
    Assume(setindex < quality_clusters.size());

    // Extract the Cluster-owning unique_ptr.
    std::unique_ptr<Cluster> ret = std::move(quality_clusters[setindex]);
    ret->m_quality = QualityLevel::NONE;
    ret->m_setindex = ClusterSetIndex(-1);

    // Clean up space in quality_cluster.
    auto max_setindex = quality_clusters.size() - 1;
    if (setindex != max_setindex) {
        // If the cluster was not the last element of quality_clusters, move that to take its place.
        quality_clusters.back()->m_setindex = setindex;
        quality_clusters[setindex] = std::move(quality_clusters.back());
    }
    // The last element of quality_clusters is now unused; drop it.
    quality_clusters.pop_back();

    return ret;
}

ClusterSetIndex TxGraphImpl::InsertCluster(std::unique_ptr<Cluster>&& cluster, QualityLevel quality) noexcept
{
    // Cannot insert with quality level NONE (as that would mean not inserted).
    Assume(quality != QualityLevel::NONE);
    // The passed-in Cluster must not currently be in the TxGraphImpl.
    Assume(cluster->m_quality == QualityLevel::NONE);

    // Append it at the end of the relevant TxGraphImpl::m_cluster.
    auto& quality_clusters = m_clusters[int(quality)];
    ClusterSetIndex ret = quality_clusters.size();
    cluster->m_quality = quality;
    cluster->m_setindex = ret;
    quality_clusters.push_back(std::move(cluster));
    return ret;
}

void TxGraphImpl::SetClusterQuality(QualityLevel old_quality, ClusterSetIndex old_index, QualityLevel new_quality) noexcept
{
    Assume(new_quality != QualityLevel::NONE);

    // Don't do anything if the quality did not change.
    if (old_quality == new_quality) return;
    // Extract the cluster from where it currently resides.
    auto cluster_ptr = ExtractCluster(old_quality, old_index);
    // And re-insert it where it belongs.
    InsertCluster(std::move(cluster_ptr), new_quality);
}

void TxGraphImpl::DeleteCluster(Cluster& cluster) noexcept
{
    // Extract the cluster from where it currently resides.
    auto cluster_ptr = ExtractCluster(cluster.m_quality, cluster.m_setindex);
    // And throw it away.
    cluster_ptr.reset();
}

void TxGraphImpl::ApplyRemovals() noexcept
{
    auto& to_remove = m_to_remove;
    // Skip if there is nothing to remove.
    if (to_remove.empty()) return;
    // Group the set of to-be-removed entries by Cluster*.
    std::sort(m_to_remove.begin(), m_to_remove.end(), [&](GraphIndex a, GraphIndex b) noexcept {
        return std::less{}(m_entries[a].m_locator.cluster, m_entries[b].m_locator.cluster);
    });
    // Process per Cluster.
    std::span to_remove_span{m_to_remove};
    while (!to_remove_span.empty()) {
        Cluster* cluster = m_entries[to_remove_span.front()].m_locator.cluster;
        if (cluster != nullptr) {
            // If the first to_remove_span entry's Cluster exists, hand to_remove_span to it, so it
            // can pop off whatever applies to it.
            cluster->ApplyRemovals(*this, to_remove_span);
        } else {
            // Otherwise, skip this already-removed entry. This may happen when RemoveTransaction
            // was called twice on the same Ref.
            to_remove_span = to_remove_span.subspan(1);
        }
    }
    m_to_remove.clear();
    Compact();
}

void TxGraphImpl::SwapIndexes(GraphIndex a, GraphIndex b) noexcept
{
    Assume(a < m_entries.size());
    Assume(b < m_entries.size());
    // Swap the Entry objects.
    std::swap(m_entries[a], m_entries[b]);
    // Iterate over both objects.
    for (int i = 0; i < 2; ++i) {
        GraphIndex idx = i ? b : a;
        Entry& entry = m_entries[idx];
        // Update linked Ref.
        if (entry.m_ref) GetRefIndex(*entry.m_ref) = idx;
        // Update the locator. The rest of the Entry information will not change, so no need to
        // invoke Cluster::Updated().
        Locator& locator = entry.m_locator;
        if (locator.IsPresent()) {
            locator.cluster->UpdateMapping(locator.index, idx);
        }
    }
}

void TxGraphImpl::Compact() noexcept
{
    // We cannot compact while any to-be-applied operations remain, as we'd need to rewrite them.
    // It is easier to delay the compaction until they have been applied.
    if (!m_deps_to_add.empty()) return;
    if (!m_to_remove.empty()) return;

    // Sort the GraphIndexes that need to be cleaned up. They are sorted in reverse, so the last
    // ones get processed first. This means earlier-processed GraphIndexes will not cause moving of
    // later-processed ones during the "swap with end of m_entries" step below (which might
    // invalidate them).
    std::sort(m_unlinked.begin(), m_unlinked.end(), std::greater{});

    auto last = GraphIndex(-1);
    for (GraphIndex idx : m_unlinked) {
        // m_unlinked should never contain the same GraphIndex twice (the code below would fail
        // if so, because GraphIndexes get invalidated by removing them).
        Assume(idx != last);
        last = idx;

        // Make sure the entry is unlinked.
        Entry& entry = m_entries[idx];
        Assume(entry.m_ref == nullptr);
        // Make sure the entry does not occur in the graph.
        Assume(!entry.m_locator.IsPresent());

        // Move the entry to the end.
        if (idx != m_entries.size() - 1) SwapIndexes(idx, m_entries.size() - 1);
        // Drop the entry for idx, now that it is at the end.
        m_entries.pop_back();
    }
    m_unlinked.clear();
}

void TxGraphImpl::Split(Cluster& cluster) noexcept
{
    // To split a Cluster, first make sure all removals are applied (as we might need to split
    // again afterwards otherwise).
    ApplyRemovals();
    bool del = cluster.Split(*this);
    if (del) {
        // Cluster::Split reports whether the Cluster is to be deleted.
        DeleteCluster(cluster);
    }
}

void TxGraphImpl::SplitAll() noexcept
{
    // Before splitting all Cluster, first make sure all removals are applied.
    ApplyRemovals();
    auto& queue = m_clusters[int(QualityLevel::NEEDS_SPLIT)];
    while (!queue.empty()) {
        Split(*queue.back().get());
    }
}

void TxGraphImpl::GroupClusters() noexcept
{
    // If the groupings have been computed already, nothing is left to be done.
    if (m_group_data.has_value()) return;

    // Before computing which Clusters need to be merged together, first apply all removals and
    // split the Clusters into connected components. If we would group first, we might end up
    // with inefficient Clusters which just end up being split again anyway.
    SplitAll();

    /** Annotated clusters: an entry for each Cluster, together with the representative for the
     *  partition it is in if known, or with nullptr if not yet known. */
    std::vector<std::pair<Cluster*, Cluster*>> an_clusters;
    /** Annotated dependencies: an entry for each m_deps_to_add entry (excluding ones that apply
     *  to removed transactions), together with the representative root of the partition of
     *  Clusters it applies to. */
    std::vector<std::pair<std::pair<GraphIndex, GraphIndex>, Cluster*>> an_deps;

    // Construct a an_clusters entry for every parent and child in the to-be-applied dependencies.
    for (const auto& [par, chl] : m_deps_to_add) {
        auto par_cluster = m_entries[par].m_locator.cluster;
        auto chl_cluster = m_entries[chl].m_locator.cluster;
        // Skip dependencies for which the parent or child transaction is removed.
        if (par_cluster == nullptr || chl_cluster == nullptr) continue;
        an_clusters.emplace_back(par_cluster, nullptr);
        // Do not include a duplicate when parent and child are identical, as it'll be removed
        // below anyway.
        if (chl_cluster != par_cluster) an_clusters.emplace_back(chl_cluster, nullptr);
    }
    // Sort and deduplicate an_clusters, so we end up with a sorted list of all involved Clusters
    // to which dependencies apply.
    std::sort(an_clusters.begin(), an_clusters.end());
    an_clusters.erase(std::unique(an_clusters.begin(), an_clusters.end()), an_clusters.end());

    // Run the union-find algorithm to to find partitions of the input Clusters which need to be
    // grouped together. See https://en.wikipedia.org/wiki/Disjoint-set_data_structure.
    {
        /** Each PartitionData entry contains information about a single input Cluster. */
        struct PartitionData
        {
            /** The cluster this holds information for. */
            Cluster* cluster;
            /** All PartitionData entries belonging to the same partition are organized in a tree.
             *  Each element points to its parent, or to itself if it is the root. The root is then
             *  a representative for the entire tree, and can be found by walking upwards from any
             *  element. */
            PartitionData* parent;
            /** (only if this is a root, so when parent == this) An upper bound on the height of
             *  tree for this partition. */
            unsigned rank;
        };
        /** Information about each input Cluster. Sorted by Cluster* pointer. */
        std::vector<PartitionData> partition_data;

        /** Given a Cluster, find its corresponding PartitionData. */
        auto locate_fn = [&](Cluster* arg) noexcept -> PartitionData* {
            auto it = std::lower_bound(partition_data.begin(), partition_data.end(), arg,
                                       [](auto& a, Cluster* ptr) noexcept { return a.cluster < ptr; });
            Assume(it != partition_data.end());
            Assume(it->cluster == arg);
            return &*it;
        };

        /** Given a PartitionData, find the root of the tree it is in (its representative). */
        static constexpr auto find_root_fn = [](PartitionData* data) noexcept -> PartitionData* {
            while (data->parent != data) {
                // Replace pointers to parents with pointers to grandparents.
                // See https://en.wikipedia.org/wiki/Disjoint-set_data_structure#Finding_set_representatives.
                auto par = data->parent;
                data->parent = par->parent;
                data = par;
            }
            return data;
        };

        /** Given two PartitionDatas, union the partitions they are in. */
        static constexpr auto union_fn = [](PartitionData* arg1, PartitionData* arg2) noexcept {
            // Find the roots of the trees, and bail out if they are already equal (which would
            // mean they are in the same partition already).
            auto rep1 = find_root_fn(arg1);
            auto rep2 = find_root_fn(arg2);
            if (rep1 == rep2) return;
            // Pick the lower-rank root to become a child of the higher-rank one.
            // See https://en.wikipedia.org/wiki/Disjoint-set_data_structure#Union_by_rank.
            if (rep1->rank < rep2->rank) std::swap(rep1, rep2);
            rep2->parent = rep1;
            rep1->rank += (rep1->rank == rep2->rank);
        };

        // Start by initializing every Cluster as its own singleton partition.
        partition_data.resize(an_clusters.size());
        for (size_t i = 0; i < an_clusters.size(); ++i) {
            partition_data[i].cluster = an_clusters[i].first;
            partition_data[i].parent = &partition_data[i];
            partition_data[i].rank = 0;
        }

        // Run through all parent/child pairs in m_deps_to_add, and union the
        // the partitions their Clusters are in.
        for (const auto& [par, chl] : m_deps_to_add) {
            auto par_cluster = m_entries[par].m_locator.cluster;
            auto chl_cluster = m_entries[chl].m_locator.cluster;
            // Nothing to do if parent and child are in the same Cluster.
            if (par_cluster == chl_cluster) continue;
            // Nothing to do if either parent or child transaction is removed already.
            if (par_cluster == nullptr || chl_cluster == nullptr) continue;
            Assume(par != chl);
            union_fn(locate_fn(par_cluster), locate_fn(chl_cluster));
        }

        // Populate the an_clusters and an_deps data structures with the list of input Clusters,
        // and the input dependencies, annotated with the representative of the Cluster partition
        // it applies to.
        for (size_t i = 0; i < partition_data.size(); ++i) {
            auto& data = partition_data[i];
            // Find the representative of the partition Cluster i is in, and store it with the
            // Cluster.
            auto rep = find_root_fn(&data)->cluster;
            Assume(an_clusters[i].second == nullptr);
            an_clusters[i].second = rep;
        }
        an_deps.reserve(m_deps_to_add.size());
        for (auto [par, chl] : m_deps_to_add) {
            auto chl_cluster = m_entries[chl].m_locator.cluster;
            auto par_cluster = m_entries[par].m_locator.cluster;
            // Nothing to do if either parent or child transaction is removed already.
            if (par_cluster == nullptr || chl_cluster == nullptr) continue;
            // Find the representative of the partition which this dependency's child is in (which
            // should be the same as the one for the parent).
            auto rep = find_root_fn(locate_fn(chl_cluster))->cluster;
            // Create an_deps entry.
            an_deps.emplace_back(std::pair{par, chl}, rep);
        }
    }

    // Sort both an_clusters and an_deps by representative of the partition they are in, grouping
    // all those applying to the same partition together.
    std::sort(an_deps.begin(), an_deps.end(), [](auto& a, auto& b) noexcept { return a.second < b.second; });
    std::sort(an_clusters.begin(), an_clusters.end(), [](auto& a, auto& b) noexcept { return a.second < b.second; });

    // Translate the resulting cluster groups to the m_group_data structure.
    m_group_data = std::vector<GroupEntry>{};
    auto an_deps_it = an_deps.begin();
    auto an_clusters_it = an_clusters.begin();
    while (an_clusters_it != an_clusters.end()) {
        // Process all clusters/dependencies belonging to the partition with representative rep.
        auto rep = an_clusters_it->second;
        // Create and initialize a new GroupData entry for the partition.
        auto& new_entry = m_group_data->emplace_back();
        // Add all its clusters to it (copying those from an_clusters to m_clusters).
        while (an_clusters_it != an_clusters.end() && an_clusters_it->second == rep) {
            new_entry.m_clusters.push_back(an_clusters_it->first);
            ++an_clusters_it;
        }
        // Add all its dependencies to it (copying those back from an_deps to m_deps).
        while (an_deps_it != an_deps.end() && an_deps_it->second == rep) {
            new_entry.m_deps.push_back(an_deps_it->first);
            ++an_deps_it;
        }
    }
    Assume(an_deps_it == an_deps.end());
    Assume(an_clusters_it == an_clusters.end());
    Compact();
}

void TxGraphImpl::Merge(std::span<Cluster*> to_merge) noexcept
{
    Assume(!to_merge.empty());
    // Nothing to do if a group consists of just a single Cluster.
    if (to_merge.size() == 1) return;

    // Move the largest Cluster to the front of to_merge. As all transactions in other to-be-merged
    // Clusters will be moved to that one, putting the largest one first minimizes the number of
    // moves.
    size_t max_size_pos{0};
    DepGraphIndex max_size = to_merge[max_size_pos]->GetTxCount();
    for (size_t i = 1; i < to_merge.size(); ++i) {
        DepGraphIndex size = to_merge[i]->GetTxCount();
        if (size > max_size) {
            max_size_pos = i;
            max_size = size;
        }
    }
    if (max_size_pos != 0) std::swap(to_merge[0], to_merge[max_size_pos]);

    // Merge all further Clusters in the group into the first one, and delete them.
    for (size_t i = 1; i < to_merge.size(); ++i) {
        to_merge[0]->Merge(*this, *to_merge[i]);
        DeleteCluster(*to_merge[i]);
    }
}

void TxGraphImpl::ApplyDependencies() noexcept
{
    // Compute the groups of to-be-merged Clusters (which also applies all removals, and splits).
    GroupClusters();
    Assume(m_group_data.has_value());
    // Nothing to do if there are no dependencies to be added.
    if (m_deps_to_add.empty()) return;

    // For each group of to-be-merged Clusters.
    for (auto& group_data : *m_group_data) {
        // Invoke Merge() to merge them into a single Cluster.
        Merge(group_data.m_clusters);
        // Actually apply all to-be-added dependencies (all parents and children from this grouping
        // belong to the same Cluster at this point because of the merging above).
        const auto& loc = m_entries[group_data.m_deps[0].second].m_locator;
        Assume(loc.IsPresent());
        loc.cluster->ApplyDependencies(*this, group_data.m_deps);
    }

    // Wipe the list of to-be-added dependencies now that they are applied.
    m_deps_to_add.clear();
    Compact();
    // Also no further Cluster mergings are needed (note that we clear, but don't set to
    // std::nullopt, as that would imply the groupings are unknown).
    m_group_data = std::vector<GroupEntry>{};
}

void Cluster::Relinearize(TxGraphImpl& graph, uint64_t max_iters) noexcept
{
    // We can only relinearize Clusters that do not need splitting.
    Assume(!NeedsSplitting());
    // No work is required for Clusters which are already optimally linearized.
    if (IsOptimal()) return;
    // Invoke the actual linearization algorithm (passing in the existing one).
    uint64_t rng_seed = graph.m_rng.rand64();
    auto [linearization, optimal] = Linearize(m_depgraph, max_iters, rng_seed, m_linearization);
    // Postlinearize if the result isn't optimal already. This guarantees (among other things)
    // that the chunks of the resulting linearization are all connected.
    if (!optimal) PostLinearize(m_depgraph, linearization);
    // Update the linearization.
    m_linearization = std::move(linearization);
    // Update the Cluster's quality.
    auto new_quality = optimal ? QualityLevel::OPTIMAL : QualityLevel::ACCEPTABLE;
    graph.SetClusterQuality(m_quality, m_setindex, new_quality);
    // Update the Entry objects.
    Updated(graph);
}

void TxGraphImpl::MakeAcceptable(Cluster& cluster) noexcept
{
    // Relinearize the Cluster if needed.
    if (!cluster.NeedsSplitting() && !cluster.IsAcceptable()) {
        cluster.Relinearize(*this, 10000);
    }
}

Cluster::Cluster(TxGraphImpl& graph, const FeePerWeight& feerate, GraphIndex graph_index) noexcept
{
    // Create a new transaction in the DepGraph, and remember its position in m_mapping.
    auto cluster_idx = m_depgraph.AddTransaction(feerate);
    m_mapping.push_back(graph_index);
    m_linearization.push_back(cluster_idx);
}

TxGraph::Ref TxGraphImpl::AddTransaction(const FeePerWeight& feerate) noexcept
{
    // Construct a new Ref.
    Ref ret;
    // Construct a new Entry, and link it with the Ref.
    auto idx = m_entries.size();
    m_entries.emplace_back();
    auto& entry = m_entries.back();
    entry.m_ref = &ret;
    GetRefGraph(ret) = this;
    GetRefIndex(ret) = idx;
    // Construct a new singleton Cluster (which is necessarily optimally linearized).
    auto cluster = std::make_unique<Cluster>(*this, feerate, idx);
    auto cluster_ptr = cluster.get();
    InsertCluster(std::move(cluster), QualityLevel::OPTIMAL);
    cluster_ptr->Updated(*this);
    ++m_txcount;
    // Return the Ref.
    return ret;
}

void TxGraphImpl::RemoveTransaction(const Ref& arg) noexcept
{
    // Don't do anything if the Ref is empty (which may be indicative of the transaction already
    // having been removed).
    if (GetRefGraph(arg) == nullptr) return;
    Assume(GetRefGraph(arg) == this);
    // Find the Cluster the transaction is in, and stop if it isn't in any.
    auto cluster = m_entries[GetRefIndex(arg)].m_locator.cluster;
    if (cluster == nullptr) return;
    // Remember that the transaction is to be removed.
    m_to_remove.push_back(GetRefIndex(arg));
    // Wipe m_group_data (as it will need to be recomputed).
    m_group_data.reset();
}

void TxGraphImpl::AddDependency(const Ref& parent, const Ref& child) noexcept
{
    // Don't do anything if either Ref is empty (which may be indicative of it having already been
    // removed).
    if (GetRefGraph(parent) == nullptr || GetRefGraph(child) == nullptr) return;
    Assume(GetRefGraph(parent) == this && GetRefGraph(child) == this);
    // Don't do anything if this is a dependency on self.
    if (GetRefIndex(parent) == GetRefIndex(child)) return;
    // Find the Cluster the parent and child transaction are in, and stop if either appears to be
    // already removed.
    auto par_cluster = m_entries[GetRefIndex(parent)].m_locator.cluster;
    if (par_cluster == nullptr) return;
    auto chl_cluster = m_entries[GetRefIndex(child)].m_locator.cluster;
    if (chl_cluster == nullptr) return;
    // Remember that this dependency is to be applied.
    m_deps_to_add.emplace_back(GetRefIndex(parent), GetRefIndex(child));
    // Wipe m_group_data (as it will need to be recomputed).
    m_group_data.reset();
}

bool TxGraphImpl::Exists(const Ref& arg) noexcept
{
    if (GetRefGraph(arg) == nullptr) return false;
    Assume(GetRefGraph(arg) == this);
    // Make sure the transaction isn't scheduled for removal.
    ApplyRemovals();
    return m_entries[GetRefIndex(arg)].m_locator.IsPresent();
}

std::vector<TxGraph::Ref*> Cluster::GetAncestorRefs(const TxGraphImpl& graph, DepGraphIndex idx) noexcept
{
    std::vector<TxGraph::Ref*> ret;
    ret.reserve(m_depgraph.Ancestors(idx).Count());
    // Translate all ancestors (in arbitrary order) to Refs (if they have any), and return them.
    for (auto idx : m_depgraph.Ancestors(idx)) {
        const auto& entry = graph.m_entries[m_mapping[idx]];
        ret.push_back(entry.m_ref);
    }
    return ret;
}

std::vector<TxGraph::Ref*> Cluster::GetDescendantRefs(const TxGraphImpl& graph, DepGraphIndex idx) noexcept
{
    std::vector<TxGraph::Ref*> ret;
    ret.reserve(m_depgraph.Descendants(idx).Count());
    // Translate all descendants (in arbitrary order) to Refs (if they have any), and return them.
    for (auto idx : m_depgraph.Descendants(idx)) {
        const auto& entry = graph.m_entries[m_mapping[idx]];
        ret.push_back(entry.m_ref);
    }
    return ret;
}

std::vector<TxGraph::Ref*> Cluster::GetClusterRefs(const TxGraphImpl& graph) noexcept
{
    std::vector<TxGraph::Ref*> ret;
    ret.reserve(m_linearization.size());
    // Translate all transactions in the Cluster (in linearization order) to Refs.
    for (auto idx : m_linearization) {
        const auto& entry = graph.m_entries[m_mapping[idx]];
        ret.push_back(entry.m_ref);
    }
    return ret;
}

FeePerWeight Cluster::GetIndividualFeerate(DepGraphIndex idx) noexcept
{
    return FeePerWeight::FromFeeFrac(m_depgraph.FeeRate(idx));
}

std::vector<TxGraph::Ref*> TxGraphImpl::GetAncestors(const Ref& arg) noexcept
{
    // Return the empty vector if the Ref is empty.
    if (GetRefGraph(arg) == nullptr) return {};
    Assume(GetRefGraph(arg) == this);
    // Apply all removals and dependencies, as the result might be incorrect otherwise.
    ApplyDependencies();
    // Find the Cluster the argument is in, and return the empty vector if it isn't in any.
    auto cluster = m_entries[GetRefIndex(arg)].m_locator.cluster;
    if (cluster == nullptr) return {};
    // Dispatch to the Cluster.
    return cluster->GetAncestorRefs(*this, m_entries[GetRefIndex(arg)].m_locator.index);
}

std::vector<TxGraph::Ref*> TxGraphImpl::GetDescendants(const Ref& arg) noexcept
{
    // Return the empty vector if the Ref is empty.
    if (GetRefGraph(arg) == nullptr) return {};
    Assume(GetRefGraph(arg) == this);
    // Apply all removals and dependencies, as the result might be incorrect otherwise.
    ApplyDependencies();
    // Find the Cluster the argument is in, and return the empty vector if it isn't in any.
    auto cluster = m_entries[GetRefIndex(arg)].m_locator.cluster;
    if (cluster == nullptr) return {};
    // Dispatch to the Cluster.
    return cluster->GetDescendantRefs(*this, m_entries[GetRefIndex(arg)].m_locator.index);
}

std::vector<TxGraph::Ref*> TxGraphImpl::GetCluster(const Ref& arg) noexcept
{
    // Return the empty vector if the Ref is empty.
    if (GetRefGraph(arg) == nullptr) return {};
    Assume(GetRefGraph(arg) == this);
    // Apply all removals and dependencies, as the result might be incorrect otherwise.
    ApplyDependencies();
    // Find the Cluster the argument is in, and return the empty vector if it isn't in any.
    auto cluster = m_entries[GetRefIndex(arg)].m_locator.cluster;
    if (cluster == nullptr) return {};
    // Make sure the Cluster has an acceptable quality level, and then dispatch to it.
    MakeAcceptable(*cluster);
    return cluster->GetClusterRefs(*this);
}

TxGraph::GraphIndex TxGraphImpl::GetTransactionCount() noexcept
{
    ApplyRemovals();
    return m_txcount;
}

FeePerWeight TxGraphImpl::GetIndividualFeerate(const Ref& arg) noexcept
{
    // Return the empty FeePerWeight if the passed Ref is empty.
    if (GetRefGraph(arg) == nullptr) return {};
    Assume(GetRefGraph(arg) == this);
    // Apply removals, so that we can correctly report FeePerWeight{} for non-existing transaction.
    ApplyRemovals();
    // Find the cluster the argument is in, and return the empty FeePerWeight if it isn't in any.
    auto cluster = m_entries[GetRefIndex(arg)].m_locator.cluster;
    if (cluster == nullptr) return {};
    // Dispatch to the Cluster.
    return cluster->GetIndividualFeerate(m_entries[GetRefIndex(arg)].m_locator.index);
}

void Cluster::SetFee(TxGraphImpl& graph, DepGraphIndex idx, int64_t fee) noexcept
{
    // Make sure the specified DepGraphIndex exists in this Cluster.
    Assume(m_depgraph.Positions()[idx]);
    // Bail out if the fee isn't actually being changed.
    if (m_depgraph.FeeRate(idx).fee == fee) return;
    // Update the fee, remember that relinearization will be necessary, and update the Entries
    // in the same Cluster.
    m_depgraph.FeeRate(idx).fee = fee;
    if (!NeedsSplitting()) {
        graph.SetClusterQuality(m_quality, m_setindex, QualityLevel::NEEDS_RELINEARIZE);
    }
    Updated(graph);
}

void TxGraphImpl::SetTransactionFee(const Ref& ref, int64_t fee) noexcept
{
    // Don't do anything if the passed Ref is empty.
    if (GetRefGraph(ref) == nullptr) return;
    Assume(GetRefGraph(ref) == this);
    // Find the entry, its locator, and inform its Cluster about the new feerate, if any.
    auto& entry = m_entries[GetRefIndex(ref)];
    auto& locator = entry.m_locator;
    if (locator.IsPresent()) {
        locator.cluster->SetFee(*this, locator.index, fee);
    }
}

} // namespace

TxGraph::Ref::~Ref()
{
    if (m_graph) {
        // Inform the TxGraph about the Ref being destroyed.
        m_graph->UnlinkRef(m_index);
        m_graph = nullptr;
    }
}

TxGraph::Ref& TxGraph::Ref::operator=(Ref&& other) noexcept
{
    // Unlink the current graph, if any.
    if (m_graph) m_graph->UnlinkRef(m_index);
    // Inform the other's graph about the move, if any.
    if (other.m_graph) other.m_graph->UpdateRef(other.m_index, *this);
    // Actually update the contents.
    m_graph = other.m_graph;
    m_index = other.m_index;
    other.m_graph = nullptr;
    other.m_index = GraphIndex(-1);
    return *this;
}

TxGraph::Ref::Ref(Ref&& other) noexcept
{
    // Inform the TxGraph of other that its Ref is being moved.
    if (other.m_graph) other.m_graph->UpdateRef(other.m_index, *this);
    // Actually move the contents.
    std::swap(m_graph, other.m_graph);
    std::swap(m_index, other.m_index);
}

std::unique_ptr<TxGraph> MakeTxGraph() noexcept
{
    return std::make_unique<TxGraphImpl>();
}
