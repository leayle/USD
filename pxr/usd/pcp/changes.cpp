//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
/// \file Changes.cpp

#include "pxr/pxr.h"
#include "pxr/usd/pcp/changes.h"
#include "pxr/usd/pcp/cache.h"
#include "pxr/usd/pcp/debugCodes.h"
#include "pxr/usd/pcp/dependencies.h"
#include "pxr/usd/pcp/instancing.h"
#include "pxr/usd/pcp/layerStack.h"
#include "pxr/usd/pcp/layerStackRegistry.h"
#include "pxr/usd/pcp/pathTranslation.h"
#include "pxr/usd/pcp/utils.h"
#include "pxr/usd/sdf/changeList.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/ar/resolverContextBinder.h"
#include "pxr/base/trace/trace.h"

PXR_NAMESPACE_OPEN_SCOPE

static
void
Pcp_SubsumeDescendants(SdfPathSet* pathSet)
{
    SdfPathSet::iterator prefixIt = pathSet->begin(), end = pathSet->end();
    while (prefixIt != end) {
        // Find the range of paths under path *prefixIt.
        SdfPathSet::iterator first = prefixIt;
        SdfPathSet::iterator last  = ++first;
        while (last != end && last->HasPrefix(*prefixIt)) {
            ++last;
        }

        // Remove the range.
        pathSet->erase(first, last);

        // Next path is not under previous path.
        prefixIt = last;
    }
}

void
Pcp_SubsumeDescendants(SdfPathSet* pathSet, const SdfPath& prefix)
{
    // Start at first path in pathSet that is prefix or greater.
    SdfPathSet::iterator first = pathSet->lower_bound(prefix); 

    // Scan for next path in pathSet that does not have prefix as a prefix.
    SdfPathSet::iterator last = first;
    SdfPathSet::iterator end = pathSet->end();
    while (last != end && last->HasPrefix(prefix)) {
        ++last;
    }

    // Erase the paths in the range.
    pathSet->erase(first, last);
}

PcpLifeboat::PcpLifeboat()
{
    // Do nothing
}

PcpLifeboat::~PcpLifeboat()
{
    // Do nothing
}

void
PcpLifeboat::Retain(const SdfLayerRefPtr& layer)
{
    _layers.insert(layer);
}

void
PcpLifeboat::Retain(const PcpLayerStackRefPtr& layerStack)
{
    _layerStacks.insert(layerStack);
}

const std::set<PcpLayerStackRefPtr>& 
PcpLifeboat::GetLayerStacks() const
{
    return _layerStacks;
}

void
PcpLifeboat::Swap(PcpLifeboat& other)
{
    std::swap(_layers, other._layers);
    std::swap(_layerStacks, other._layerStacks);
}

PcpChanges::PcpChanges()
{
    // Do nothing
}

PcpChanges::~PcpChanges()
{
    // Do nothing
}

#define PCP_APPEND_DEBUG(...)                       \
    if (!debugSummary) ; else                    \
        *debugSummary += TfStringPrintf(__VA_ARGS__)

enum Pcp_ChangesLayerStackChange {
    Pcp_ChangesLayerStackChangeNone,
    Pcp_ChangesLayerStackChangeSignificant,
    Pcp_ChangesLayerStackChangeMaybeSignificant
};

static
Pcp_ChangesLayerStackChange
Pcp_EntryRequiresLayerStackChange(const SdfChangeList::Entry& entry)
{
    // XXX: This only requires blowing the layer stacks using this
    //      identifier that haven't also been updated to use the new
    //      identifier.
    if (entry.flags.didChangeIdentifier) {
        return Pcp_ChangesLayerStackChangeSignificant;
    }

    // Order of layers in layer stack probably changed.
    // XXX: Don't return true if these changes don't affect the
    //      layer tree order.
    for (auto const &p: entry.infoChanged) {
        if (p.first == SdfFieldKeys->Owner ||
            p.first == SdfFieldKeys->SessionOwner ||
            p.first == SdfFieldKeys->HasOwnedSubLayers) {
            return Pcp_ChangesLayerStackChangeSignificant;
        }
    }

    // Layer was added or removed.
    TF_FOR_ALL(i, entry.subLayerChanges) {
        if (i->second == SdfChangeList::SubLayerAdded ||
            i->second == SdfChangeList::SubLayerRemoved) {
            // Whether the change is significant depends on whether any
            // added/removed layer is significant.  To check that we need
            // the help of each cache using this layer.
            return Pcp_ChangesLayerStackChangeMaybeSignificant;
        }
    }

    return Pcp_ChangesLayerStackChangeNone;
}

static
bool
Pcp_EntryRequiresLayerStackOffsetsChange(
    const SdfLayerHandle &layer,
    const SdfChangeList::Entry& entry,
    bool *rootLayerStacksMayNeedTcpsRecompute)
{
    // Check any changes to actual sublayer offsets.
    for (const auto &it : entry.subLayerChanges) {
        if (it.second == SdfChangeList::SubLayerOffset) {
            return true;
        }
    }

    // Check if the TCPS metadata field changed. Note that this encapsulates
    // both changes to timeCodesPerSecond and framesPerSecond as the 
    // SdfChangeManager will send a send a FPS change as a change to TCPS as 
    // well when the FPS is relevant as a fallback for an unspecified TCPS. 
    auto it = entry.FindInfoChange(SdfFieldKeys->TimeCodesPerSecond);
    if (it != entry.infoChanged.end()) {
        // The old and new values in the entry already account for the 
        // "computed TCPS" when the FPS is used as a fallback. So we still have
        // to check if the computed TCPS changed.
        // 
        // We also have to account here for the case where both the FPS and 
        // TCPS are unspecified, either before or after the change, as the old
        // or new entry value will be empty which is equivalent to specifying 
        // the TCPS fallback value from the SdfSchema.
        const VtValue &oldComputedTcps = it->second.first;
        const VtValue &newComputedTcps = it->second.second;
        auto _MatchesFallback = [&layer](const VtValue &val) {
            return layer->GetSchema().GetFallback(
                SdfFieldKeys->TimeCodesPerSecond) == val;
        };

        // If the old and new TCPS values are the same, this indicates that
        // either the old or new TCPS field is actually unauthored and is 
        // falling back to an authored FPS value. This is not a computed TCPS 
        // change for the layer itself and doesn't directly affect the offset
        // for the layer relative to other layers.
        // 
        // However, if this layer is the session or root layer of a cache's 
        // root layer stack, this change could still have an effect on the
        // computed overall TCPS of that layer stack. That's why we still flag
        // this change so we can check for this case after layer changes are
        // processed.
        // 
        // XXX: Note this requires an interpretation of the change information
        // coming out of Sdf involving knowledge of specific implementation 
        // details of Sdf change management. Ideally Sdf should provide a
        // notification differentiation between "authored TCPS" changed vs
        // "computed TCPS" changed.
        if (oldComputedTcps == newComputedTcps) {
            if (rootLayerStacksMayNeedTcpsRecompute) {
                *rootLayerStacksMayNeedTcpsRecompute = true;
            }
            return false;
        }

        // If either old or new value is empty, and the other value matches the
        // fallback, then we don't have an effective TCPS change.
        if ((oldComputedTcps.IsEmpty() && _MatchesFallback(newComputedTcps)) ||
            (newComputedTcps.IsEmpty() && _MatchesFallback(oldComputedTcps))) {
            return false;
        }
        return true;
    }
    
    return false;
}

static
bool
Pcp_EntryRequiresPrimIndexChange(const SdfChangeList::Entry& entry)
{
    // Inherits, specializes, reference or variants changed.
    if (entry.flags.didChangePrimInheritPaths ||
        entry.flags.didChangePrimSpecializes  ||
        entry.flags.didChangePrimReferences   ||
        entry.flags.didChangePrimVariantSets) {
        return true;
    }

    // Payload, permission or variant selection changed.
    // XXX: We don't require a prim graph change if:
    //        we add/remove an unrequested payload;
    //        permissions change doesn't add/remove any specs
    //            that themselves require prim graph changes;
    //        variant selection was invalid and is still invalid.
    for (auto const &p: entry.infoChanged) {
        if (p.first == SdfFieldKeys->Payload ||
            p.first == SdfFieldKeys->Permission ||
            p.first == SdfFieldKeys->VariantSelection ||
            p.first == SdfFieldKeys->Instanceable) {
            return true;
        }
    }

    return false;
}

enum {
    Pcp_EntryChangeSpecsAddInert        = 1,
    Pcp_EntryChangeSpecsRemoveInert     = 2,
    Pcp_EntryChangeSpecsAddNonInert     = 4,
    Pcp_EntryChangeSpecsRemoveNonInert  = 8,
    Pcp_EntryChangeSpecsTargets         = 16,
    Pcp_EntryChangeSpecsConnections     = 32,
    Pcp_EntryChangeSpecsAdd =
        Pcp_EntryChangeSpecsAddInert |
        Pcp_EntryChangeSpecsAddNonInert,
    Pcp_EntryChangeSpecsRemove =
        Pcp_EntryChangeSpecsRemoveInert |
        Pcp_EntryChangeSpecsRemoveNonInert,
    Pcp_EntryChangeSpecsInert =
        Pcp_EntryChangeSpecsAddInert |
        Pcp_EntryChangeSpecsRemoveInert,
    Pcp_EntryChangeSpecsNonInert =
        Pcp_EntryChangeSpecsAddNonInert |
        Pcp_EntryChangeSpecsRemoveNonInert
};

static int
Pcp_EntryRequiresPrimSpecsChange(const SdfChangeList::Entry& entry)
{
    int result = 0;

    result |= entry.flags.didAddInertPrim
              ? Pcp_EntryChangeSpecsAddInert      : 0;
    result |= entry.flags.didRemoveInertPrim
             ? Pcp_EntryChangeSpecsRemoveInert    : 0;
    result |= entry.flags.didAddNonInertPrim
             ? Pcp_EntryChangeSpecsAddNonInert    : 0;
    result |= entry.flags.didRemoveNonInertPrim
             ? Pcp_EntryChangeSpecsRemoveNonInert : 0;

    return result;
}

static int
Pcp_EntryRequiresPropertySpecsChange(const SdfChangeList::Entry& entry)
{
    int result = 0;

    result |= entry.flags.didAddPropertyWithOnlyRequiredFields
              ? Pcp_EntryChangeSpecsAddInert      : 0;
    result |= entry.flags.didRemovePropertyWithOnlyRequiredFields
             ? Pcp_EntryChangeSpecsRemoveInert    : 0;
    result |= entry.flags.didAddProperty
             ? Pcp_EntryChangeSpecsAddNonInert    : 0;
    result |= entry.flags.didRemoveProperty
             ? Pcp_EntryChangeSpecsRemoveNonInert : 0;

    if (entry.flags.didChangeRelationshipTargets) {
        result |= Pcp_EntryChangeSpecsTargets;
    }
    if (entry.flags.didChangeAttributeConnection) {
        result |= Pcp_EntryChangeSpecsConnections;
    }

    return result;
}

static bool
Pcp_EntryRequiresPropertyIndexChange(const SdfChangeList::Entry& entry)
{
    for (auto const &p: entry.infoChanged) {
        if (p.first == SdfFieldKeys->Permission) {
            return true;
        }
    }
    return false;
}

// Returns true if any changed info field in the changelist entry is a
// field that may be an input used to compute file format arguments for a 
// dynamic file format used by a prim index in the cache. This is a minimal
// filtering by field name only, ignoring all other context.
static bool
Pcp_ChangeMayAffectDynamicFileFormatArguments(
    const PcpCache* cache,
    const SdfChangeList::Entry& entry,
    std::string* debugSummary)
{
    // Early out if the cache has no dynamic file format dependencies.
    if (cache->HasAnyDynamicFileFormatArgumentDependencies()) {
        for (const auto& change : entry.infoChanged) {
            if (cache->IsPossibleDynamicFileFormatArgumentField(change.first)) {
                PCP_APPEND_DEBUG("  Info change for field '%s' may affect "
                                 "dynamic file format arguments\n",
                                 change.first.GetText());
                return true;
            }
        }
    }

    return false;
}

static bool
Pcp_PrimSpecOrDescendantHasRelocates(const SdfLayerHandle& layer, 
                                     const SdfPath& primPath)
{
    TRACE_FUNCTION();

    if (layer->HasField(primPath, SdfFieldKeys->Relocates)) {
        return true;
    }

    TfTokenVector primChildNames;
    if (layer->HasField(primPath, SdfChildrenKeys->PrimChildren,
                        &primChildNames)) {
        for (const TfToken& name : primChildNames) {
            if (Pcp_PrimSpecOrDescendantHasRelocates(
                    layer, primPath.AppendChild(name))) {
                return true;
            }
        }
    }

    return false;
}

// Returns true if any of the info changed in the change list affects the file
// format arguments of for a dynamic file format under the prim index at path. 
static bool 
Pcp_DoesInfoChangeAffectFileFormatArguments(
    PcpCache const *cache, const SdfPath& primIndexPath,
    const SdfChangeList::Entry &changes,
    std::string *debugSummary)
{
    PCP_APPEND_DEBUG(
        "Pcp_DoesInfoChangeAffectFileFormatArguments %s:%s?\n",
        cache->GetLayerStackIdentifier().rootLayer->GetIdentifier().c_str(),
        primIndexPath.GetText());

    // Get the cached  dynamic file format dependency data for the prim index.
    // This will exist if the prim index exists and has any direct arcs that 
    // used a dynamic file format.
    const PcpDynamicFileFormatDependencyData &depData =
        cache->GetDynamicFileFormatArgumentDependencyData(primIndexPath);
    if (depData.IsEmpty()) {
        PCP_APPEND_DEBUG("  Prim index has no dynamic file format dependencies\n");
        return false;
    }

    // For each info field ask the dependency data if the change can affect
    // the file format args of any node in the prim index graph.
    for (const auto& change : changes.infoChanged) {
        const bool isRelevantChange =
            depData.CanFieldChangeAffectFileFormatArguments(
                change.first, change.second.first, change.second.second);
        PCP_APPEND_DEBUG("  Field '%s' change: %s -> %s "
                         "%s relevant for prim index path '%s'\n",
                         change.first.GetText(),
                         TfStringify(change.second.first).c_str(),
                         TfStringify(change.second.second).c_str(),
                         isRelevantChange ? "IS" : "is NOT",
                         primIndexPath.GetText());
        if (isRelevantChange) {
            return true;
        }
    }

    return false;
}

// DepFunc is a function type void (const PcpDependency &)
template <typename DepFunc>
static void
Pcp_DidChangeDependents(
    const PcpCache* cache,
    const SdfLayerHandle& layer,
    const SdfPath& path,
    bool processPrimDescendants,
    bool onlyExistingDependentPaths,
    const DepFunc &processDependencyFunc,
    std::string* debugSummary)
{
    // Don't want to put a trace here, as this function can get called many
    // times during change processing.
    // TRACE_FUNCTION();

    // We don't recurse on site for property paths, only prim paths if 
    // necessary.
    const bool recurseOnSite =
        processPrimDescendants &&
        (path == SdfPath::AbsoluteRootPath()  ||
         path.IsPrimOrPrimVariantSelectionPath());
    PcpDependencyVector deps = cache->FindSiteDependencies(
        layer, path, PcpDependencyTypeAnyIncludingVirtual, recurseOnSite,
        /* recurseOnIndex */ false,
        /* filter */ onlyExistingDependentPaths);

    PCP_APPEND_DEBUG(
        "   Resync following in @%s@ %s due to Sdf site @%s@<%s>%s:\n",
        cache->GetLayerStackIdentifier()
        .rootLayer->GetIdentifier().c_str(),
        recurseOnSite ?
        "recurse on prim descendants" :
        "do not recurse on prim descendants",
        layer->GetIdentifier().c_str(), path.GetText(),
        onlyExistingDependentPaths ?
        " (restricted to existing caches)" :
        " (not restricted to existing caches)");

    // Run the process function on each found dependency.
    for (const auto& dep: deps) {
        PCP_APPEND_DEBUG(
            "    <%s> depends on <%s>\n",
            dep.indexPath.GetText(),
            dep.sitePath.GetText());

        processDependencyFunc(dep);
    }

    PCP_APPEND_DEBUG("   Resync end\n");
}

void
PcpChanges::DidChange(const TfSpan<const PcpCache*>& caches,
                      const SdfLayerChangeListVec& changes)
{
    // LayerStack changes
    static const int LayerStackLayersChange       = 1;
    static const int LayerStackOffsetsChange      = 2;
    static const int LayerStackRelocatesChange    = 4;
    static const int LayerStackSignificantChange  = 8;
    static const int LayerStackResolvedPathChange = 16;
    typedef int LayerStackChangeBitmask;
    typedef std::map<PcpLayerStackPtr, LayerStackChangeBitmask>
        LayerStackChangeMap;

    // Path changes
    static const int PathChangeSimple      = 1;
    static const int PathChangeTargets     = 2;
    static const int PathChangeConnections = 4;
    typedef int PathChangeBitmask;
    typedef std::map<SdfPath, PathChangeBitmask,
                     SdfPath::FastLessThan> PathChangeMap;
    typedef PathChangeMap::value_type PathChangeValue;

    // Spec changes
    typedef int SpecChangeBitmask;
    typedef std::map<SdfPath, SpecChangeBitmask,
                     SdfPath::FastLessThan> SpecChangesTypes;

    // Payload decorator changes
    typedef std::pair<const PcpCache*, SdfPath> CacheAndLayerPathPair;
    typedef std::vector<CacheAndLayerPathPair> CacheAndLayerPathPairVector;

    TRACE_FUNCTION();

    SdfPathSet pathsWithSignificantChanges;
    PathChangeMap pathsWithSpecChanges;
    SpecChangesTypes pathsWithSpecChangesTypes;
    SdfPathSet pathsWithRelocatesChanges;
    SdfPathVector oldPaths, newPaths;
    SdfPathSet fallbackToAncestorPaths;

    CacheAndLayerPathPairVector fieldForFileFormatArgumentsChanges;

    // As we process each layer below, we'll look for changes that
    // affect entire layer stacks, then process those in one pass
    // at the end.
    LayerStackChangeMap layerStackChangesMap;

    // Change debugging.
    std::string summary;
    std::string* debugSummary = TfDebug::IsEnabled(PCP_CHANGES) ? &summary : 0;

    PCP_APPEND_DEBUG("  Caches:\n");
    for (const PcpCache* cache: caches) {
        PCP_APPEND_DEBUG("    %s\n",
                 TfStringify(cache->GetLayerStack()->GetIdentifier()).c_str());
    }

    const bool allCachesInUsdMode = std::all_of(
        caches.begin(), caches.end(), 
        [](const PcpCache* cache) { return cache->IsUsd(); });

    // Process all changes, first looping over all layers.
    for (auto const &i: changes) {
        const SdfLayerHandle& layer     = i.first;
        const SdfChangeList& changeList = i.second;

        // PcpCaches in USD mode only cache prim indexes, so they only
        // care about prim changes. We can do a pre-scan of the entries
        // and bail early if none of the changes are for prims, skipping
        // over unnecessary work.
        if (allCachesInUsdMode) {
            using _Entries = SdfChangeList::EntryList;

            const _Entries& entries = changeList.GetEntryList();
            const bool hasPrimChanges = std::find_if(
                entries.begin(), entries.end(),
                [](const _Entries::value_type& entry) {
                    return !entry.first.ContainsPropertyElements();
                }) != entries.end();

            if (!hasPrimChanges) {
                PCP_APPEND_DEBUG(
                    "  Layer @%s@ changed:  skipping non-prim changes\n",
                    layer->GetIdentifier().c_str());
                continue;
            }
        }

        // Find every layer stack in every cache that includes 'layer'.
        // If there aren't any such layer stacks, we can ignore this change.
        typedef std::pair<const PcpCache*, PcpLayerStackPtrVector> 
            CacheLayerStacks;
        typedef std::vector<CacheLayerStacks> CacheLayerStacksVector;

        CacheLayerStacksVector cacheLayerStacks;
        for (auto cache : caches) {
            PcpLayerStackPtrVector stacks =
                cache->FindAllLayerStacksUsingLayer(layer);
            if (!stacks.empty()) {
                cacheLayerStacks.emplace_back(cache, std::move(stacks));
            }
        }
        if (cacheLayerStacks.empty()) {
            PCP_APPEND_DEBUG("  Layer @%s@ changed:  unused\n",
                             layer->GetIdentifier().c_str());
            continue;
        }

        PCP_APPEND_DEBUG("  Changes to layer %s:\n%s",
                         layer->GetIdentifier().c_str(),
                         TfStringify(changeList).c_str());

        // Reset state.
        LayerStackChangeBitmask layerStackChangeMask = 0;
        bool rootLayerStacksMayNeedTcpsRecompute = false;
        pathsWithSignificantChanges.clear();
        pathsWithSpecChanges.clear();
        pathsWithSpecChangesTypes.clear();
        pathsWithRelocatesChanges.clear();
        oldPaths.clear();
        newPaths.clear();
        fallbackToAncestorPaths.clear();
        fieldForFileFormatArgumentsChanges.clear();

        // Loop over each entry on the layer.
        for (auto const &j: changeList.GetEntryList()) {
            const SdfPath& path = j.first;
            const SdfChangeList::Entry& entry = j.second;

            // Figure out for which paths we must fallback to an ancestor.
            // These are the paths where a prim/property was added or
            // removed and any descendant.
            //
            // When adding the first spec for a prim or property, there
            // won't be any dependencies for that object yet, but we still
            // need to figure out the locations that will be affected by
            // the addition of this new object. Hence the need to fallback
            // to an ancestor to synthesize dependencies.
            //
            // When removing a prim or property spec, the fallback ancestor
            // is usually not needed because there should already be
            // dependencies registered for that object. However, in the case
            // where an object is renamed then removed in a single change
            // block, we will need the fallback ancestor because the
            // dependencies at the renamed path will not have been registered
            // yet. The fallback ancestor code won't be run in the usual
            // case anyway, so it's safe to just always set up the fallback
            // ancestor path.
            const bool fallbackToParent = 
                entry.flags.didAddInertPrim                             ||
                entry.flags.didRemoveInertPrim                          ||
                entry.flags.didAddNonInertPrim                          ||
                entry.flags.didRemoveNonInertPrim                       ||
                entry.flags.didAddProperty                              ||
                entry.flags.didRemoveProperty                           ||
                entry.flags.didAddPropertyWithOnlyRequiredFields        ||
                entry.flags.didRemovePropertyWithOnlyRequiredFields;

            if (fallbackToParent) {
                fallbackToAncestorPaths.insert(path);
            }

            if (path == SdfPath::AbsoluteRootPath()) {
                if (entry.flags.didReplaceContent) {
                    pathsWithSignificantChanges.insert(path);
                }

                // Treat a change to DefaultPrim as a resync
                // of that root prim path.
                auto i = entry.FindInfoChange(SdfFieldKeys->DefaultPrim);
                if (i != entry.infoChanged.end()) {
                    // old value.
                    TfToken token = i->second.first.GetWithDefault<TfToken>();
                    pathsWithSignificantChanges.insert(
                        SdfPath::IsValidIdentifier(token)
                        ? SdfPath::AbsoluteRootPath().AppendChild(token)
                        : SdfPath::AbsoluteRootPath());
                    // new value.
                    token = i->second.second.GetWithDefault<TfToken>();
                    pathsWithSignificantChanges.insert(
                        SdfPath::IsValidIdentifier(token)
                        ? SdfPath::AbsoluteRootPath().AppendChild(token)
                        : SdfPath::AbsoluteRootPath());
                }

                // Handle changes that require blowing the layer stack.
                switch (Pcp_EntryRequiresLayerStackChange(entry)) {
                case Pcp_ChangesLayerStackChangeMaybeSignificant:
                    layerStackChangeMask |= LayerStackLayersChange;
                    for (auto const &k: entry.subLayerChanges) {
                        if (k.second == SdfChangeList::SubLayerAdded ||
                            k.second == SdfChangeList::SubLayerRemoved) {
                            const std::string& sublayerPath = k.first;
                            const _SublayerChangeType sublayerChange = 
                                k.second == SdfChangeList::SubLayerAdded ? 
                                _SublayerAdded : _SublayerRemoved;

                            for (auto const &i: cacheLayerStacks) {
                                bool significant = false;
                                const SdfLayerRefPtr sublayer = 
                                    _LoadSublayerForChange(i.first,
                                                           layer,
                                                           sublayerPath,
                                                           sublayerChange);
                                
                                PCP_APPEND_DEBUG(
                                    "  Layer @%s@ changed sublayers\n",
                                    layer ? 
                                    layer->GetIdentifier().c_str() : "invalid");

                                _DidChangeSublayer(i.first /* cache */,
                                                   i.second /* stack */,
                                                   sublayerPath,
                                                   sublayer,
                                                   sublayerChange,
                                                   debugSummary,
                                                   &significant);
                                if (significant) {
                                    layerStackChangeMask |=
                                        LayerStackSignificantChange;
                                }
                            }
                        }
                    }
                    break;

                case Pcp_ChangesLayerStackChangeSignificant:
                    // Must blow everything
                    layerStackChangeMask |= LayerStackLayersChange |
                                            LayerStackSignificantChange;
                    pathsWithSignificantChanges.insert(path);
                    PCP_APPEND_DEBUG("  Layer @%s@ changed:  significant\n",
                                     layer->GetIdentifier().c_str());
                    break;

                case Pcp_ChangesLayerStackChangeNone:
                    // Layer stack is okay.   Handle changes that require
                    // blowing the layer stack offsets.
                    if (Pcp_EntryRequiresLayerStackOffsetsChange(layer, entry, 
                            &rootLayerStacksMayNeedTcpsRecompute)) {
                        layerStackChangeMask |= LayerStackOffsetsChange;

                        // Layer offsets are folded into the map functions
                        // for arcs that originate from a layer. So, when 
                        // offsets authored in a layer change, all indexes
                        // that depend on that layer must be significantly
                        // resync'd to regenerate those functions.
                        //
                        // XXX: If this becomes a performance issue, we could
                        //      potentially apply the same incremental updating
                        //      that's currently done for relocates.
                        pathsWithSignificantChanges.insert(path);
                        PCP_APPEND_DEBUG("  Layer @%s@ changed:  "
                                         "layer offsets (significant)\n",
                                         layer->GetIdentifier().c_str());
                    }
                    break;
                }

                if (entry.flags.didChangeResolvedPath) {
                    layerStackChangeMask |= LayerStackResolvedPathChange;
                }
            }

            // Handle changes that require a prim graph change.
            else if (path.IsPrimOrPrimVariantSelectionPath()) {
                if (entry.flags.didRename) {
                    // XXX: We don't have enough info to determine if
                    //      the changes so far are a rename in layer
                    //      stack space.  Renames in Sd are only renames
                    //      in a Pcp layer stack if all specs in the
                    //      layer stack were renamed the same way (for
                    //      and given old path, new path pair).  But we
                    //      don't know which layer stacks to check and
                    //      which caches they belong to.  For example,
                    //      if we rename in a referenced layer stack we
                    //      don't know here what caches are referencing
                    //      that layer stack.
                    //
                    //      In the future we'll probably avoid this
                    //      problem altogether by requiring clients to
                    //      do namespace edits through Csd if they want
                    //      efficient handling.  Csd will be able to
                    //      tell its PcpChanges object about the
                    //      renames directly and we won't do *any*
                    //      *handling of renames in this method.
                    //
                    //      For now we'll just treat renames as resyncs.
                    oldPaths.push_back(entry.oldPath);
                    newPaths.push_back(path);
                    PCP_APPEND_DEBUG("  Renamed @%s@<%s> to <%s>\n",
                                     layer->GetIdentifier().c_str(),
                                     entry.oldPath.GetText(), path.GetText());
                }
                if (int specChanges = Pcp_EntryRequiresPrimSpecsChange(entry)) {
                    pathsWithSpecChangesTypes[path] |= specChanges;
                }
                if (Pcp_EntryRequiresPrimIndexChange(entry)) {
                    pathsWithSignificantChanges.insert(path);
                }
                else {
                    for (const auto& c : cacheLayerStacks) {
                        const PcpCache* cache = c.first;
                        // Gather info changes that may affect dynamic file
                        // format arguments so we can check dependents on
                        // these changes later.
                        if (Pcp_ChangeMayAffectDynamicFileFormatArguments(
                                cache, entry, debugSummary)) {
                            PCP_APPEND_DEBUG(
                                "  Info change on @%s@<%s> may affect file "
                                "format arguments in cache '%s'\n",
                                layer->GetIdentifier().c_str(),
                                path.GetText(),
                                cache->GetLayerStackIdentifier().rootLayer
                                    ->GetIdentifier().c_str());

                            fieldForFileFormatArgumentsChanges.push_back(
                                CacheAndLayerPathPair(cache, path));
                        }
                    }
                }
                
                if (entry.HasInfoChange(SdfFieldKeys->Relocates)) {
                    layerStackChangeMask |= LayerStackRelocatesChange;
                }
            }
            else if (!allCachesInUsdMode) {
                // See comment above regarding PcpCaches in USD mode.
                // We also check for USD mode here to ensure we don't
                // process any non-prim changes if the changelist had
                // a mix of prim and non-prim changes.
                if (path.IsPropertyPath()) {
                    if (entry.flags.didRename) {
                        // XXX: See the comment above regarding renaming
                        //      prims.
                        oldPaths.push_back(entry.oldPath);
                        newPaths.push_back(path);
                        PCP_APPEND_DEBUG("  Renamed @%s@<%s> to <%s>\n",
                                         layer->GetIdentifier().c_str(),
                                         entry.oldPath.GetText(),path.GetText());
                    }
                    if (int specChanges =
                            Pcp_EntryRequiresPropertySpecsChange(entry)) {
                        pathsWithSpecChangesTypes[path] |= specChanges;
                    }
                    if (Pcp_EntryRequiresPropertyIndexChange(entry)) {
                        pathsWithSignificantChanges.insert(path);
                    }
                }
                else if (path.IsTargetPath()) {
                    if (entry.flags.didAddTarget) {
                        pathsWithSpecChangesTypes[path] |=
                            Pcp_EntryChangeSpecsAddInert;
                    }
                    if (entry.flags.didRemoveTarget) {
                        pathsWithSpecChangesTypes[path] |=
                            Pcp_EntryChangeSpecsRemoveInert;
                    }
                }
            }
        } // end for all entries in changelist

        // If we processed a change that may affect the TCPS of root layer 
        // stacks, we check that here.
        if (rootLayerStacksMayNeedTcpsRecompute) {
            for (const auto &i : cacheLayerStacks) {
                const PcpCache *cache = i.first;
                // We only need to check the root layer stacks of caches 
                // using this layer.
                if (const PcpLayerStackPtr& layerStack = 
                        cache->GetLayerStack()) {
                    // If the layer stack will need to recompute its TCPS 
                    // because this layer changed, then mark that layer stack
                    // will have its layer offsets change.
                    if (Pcp_NeedToRecomputeLayerStackTimeCodesPerSecond(
                            layerStack, layer)) {
                        PCP_APPEND_DEBUG("  Layer @%s@ changed:  "
                                         "root layer stack TCPS (significant)\n",
                                         layer->GetIdentifier().c_str());
                        layerStackChangesMap[layerStack] |= 
                            LayerStackOffsetsChange;
                        // This is a significant change to all prim indexes.
                        DidChangeSignificantly(cache, SdfPath::AbsoluteRootPath());
                    }
                }
            }
        }

        // Push layer stack changes to all layer stacks using this layer.
        if (layerStackChangeMask != 0) {
            for (auto const &i: cacheLayerStacks) {
                for (auto const &layerStack: i.second) {
                    layerStackChangesMap[layerStack] |= layerStackChangeMask;
                }
            }
        }

        // Handle spec changes.  What we do depends on what changes happened
        // and the cache at each path.
        //
        //  Prim:
        //     Add/remove inert     -- insignificant change (*)
        //     Add/remove non-inert -- significant change
        //
        //  Property:
        //     Add/remove inert     -- insignificant change
        //     Add/remove non-inert -- significant change
        //
        // (*) We may be adding the first prim spec or removing the last prim 
        // spec from a composed prim in this case.  We'll check for this in
        // DidChangeSpecs and upgrade to a significant change if we discover
        // this is the case.
        //
        // Note that in the below code, the order of the if statements does
        // matter, as a spec could be added, then removed (for example) within 
        // the same change.
        for (const auto& value : pathsWithSpecChangesTypes) {
            const SdfPath& path = value.first;
            if (path.IsPrimOrPrimVariantSelectionPath()) {
                if (value.second & Pcp_EntryChangeSpecsNonInert) {
                    pathsWithSignificantChanges.insert(path);
                }
                else if (value.second & Pcp_EntryChangeSpecsInert) {
                    pathsWithSpecChanges[path] |= PathChangeSimple;
                }
            }
            else {
                if (value.second & Pcp_EntryChangeSpecsNonInert) {
                    pathsWithSignificantChanges.insert(path);
                }
                else if (value.second & Pcp_EntryChangeSpecsInert) {
                    pathsWithSpecChanges[path] |= PathChangeSimple;
                }

                if (value.second & Pcp_EntryChangeSpecsTargets) {
                    pathsWithSpecChanges[path] |= PathChangeTargets;
                }
                if (value.second & Pcp_EntryChangeSpecsConnections) {
                    pathsWithSpecChanges[path] |= PathChangeConnections;
                }
            }
        }

        // For every path we've found on this layer that has a
        // significant change, find all paths in the cache that use the
        // spec at (layer, path) and mark them as affected.
        for (const auto& path : pathsWithSignificantChanges) {
            const bool onlyExistingDependentPaths =
                fallbackToAncestorPaths.count(path) == 0;
            for (auto cache : caches) {
                // For significant changes to a prim (as opposed to property), 
                // we need to process its dependencies as well as dependencies 
                // on descendants of that prim.
                //
                // This is needed to accommodate relocates, specifically the 
                // case where a descendant of the changed prim was relocated out
                // from beneath it. In this case, dependencies on that 
                // descendant will be in a different branch of namespace than 
                // the dependencies on the changed prim. We need to mark both 
                // sets of dependencies as being changed.
                //
                // We don't need to do this for significant property changes as
                // properties can't be individually relocated.
                Pcp_DidChangeDependents(
                    cache, layer, path, /*processPrimDescendants*/ true, 
                    onlyExistingDependentPaths, 
                    [this, &cache](const PcpDependency &dep) {
                        DidChangeSignificantly(cache, dep.indexPath);
                    },
                    debugSummary);
            }
        }

        // For every (layer, path) site we've found that has a change 
        // to a field that a prim index that generates dynamic file format 
        // arguments cares about, find all paths in the cache that depend on 
        // that site and register a significant change if the file format says 
        // the field change affects how it generates arguments.
        for (const auto& p : fieldForFileFormatArgumentsChanges) {
            const bool onlyExistingDependentPaths =
                fallbackToAncestorPaths.count(p.second) == 0;

            const PcpCache* cache = p.first;
            const SdfPath &changedPath = p.second;

            SdfChangeList::Entry const &changes =
                changeList.GetEntry(changedPath);
            // We need to recurse on prim descendants for dynamic file format
            // argument changes. This is to catch the case where there's
            // a reference to a subroot prim who has an ancestor with a dynamic
            // file format dependency. Changes that affect the ancestor may 
            // affect the descendant prim's prim index but that dependency will
            // be stored with the descendant as the ancestor prim index is not
            // itself cached when it is only used to compute subroot references.
            Pcp_DidChangeDependents(
                cache, layer, p.second, /*processPrimDescendants*/ true, 
                onlyExistingDependentPaths,
                [this, &cache, &changes, &debugSummary](
                    const PcpDependency &dep) {
                    if (Pcp_DoesInfoChangeAffectFileFormatArguments(
                            cache, dep.indexPath, changes, debugSummary)) {
                        DidChangeSignificantly(cache, dep.indexPath);
                    }
                },
                debugSummary);
        }

        // For every non-inert prim spec that has been added to this layer,
        // check if it or any of its descendant prim specs contains relocates.
        // If so, all dependent layer stacks need to recompute its cached
        // relocates. We can skip this if all caches are in USD mode, since 
        // relocates are disabled for those caches.
        if (!allCachesInUsdMode) {
            for (const auto& value : pathsWithSpecChangesTypes) {
                const SdfPath& path = value.first;
                if (!path.IsPrimOrPrimVariantSelectionPath() || 
                    !(value.second & Pcp_EntryChangeSpecsAddNonInert)) {
                    continue;
                }

                if (Pcp_PrimSpecOrDescendantHasRelocates(layer, path)) {
                    for (const CacheLayerStacks &i: cacheLayerStacks) {
                        if (i.first->IsUsd()) {
                            // No relocations in usd mode
                            continue;
                        }
                        for (const PcpLayerStackPtr &layerStack: i.second) {
                            layerStackChangesMap[layerStack] 
                                |= LayerStackRelocatesChange;
                        }
                    }
                    break;
                }
            }
        }

        // For every path we've found that has a significant change,
        // check layer stacks that have discovered relocations that
        // could be affected by that change. We can skip this if all caches
        // are in USD mode, since relocates are disabled for those caches.
        if (!pathsWithSignificantChanges.empty() && !allCachesInUsdMode) {
            // If this scope turns out to be expensive, we should look
            // at switching PcpLayerStack's _relocatesPrimPaths from
            // a std::vector to a path set.  _AddRelocateEditsForLayerStack
            // also does a traversal and might see a similar benefit.
            TRACE_SCOPE("PcpChanges::DidChange -- Checking layer stack "
                        "relocations against significant prim resyncs");

            for(const CacheLayerStacks &i: cacheLayerStacks) {
                if (i.first->IsUsd()) {
                    // No relocations in usd mode
                    continue;
                }
                for(const PcpLayerStackPtr &layerStack: i.second) {
                    const SdfPathVector& reloPaths =
                        layerStack->GetPathsToPrimsWithRelocates();
                    if (reloPaths.empty()) {
                        continue;
                    }
                    for(const SdfPath &changedPath:
                        pathsWithSignificantChanges) {
                        for(const SdfPath &reloPath: reloPaths) {
                            if (reloPath.HasPrefix(changedPath)) {
                                layerStackChangesMap[layerStack]
                                    |= LayerStackRelocatesChange;
                                goto doneWithLayerStack;
                            }
                        }
                    }
                    doneWithLayerStack:
                    ;
                }
            }
        }

        // For every path we've found on this layer that maybe requires
        // rebuilding the property stack based on parent dependencies, find
        // all paths in the cache that use the spec at (layer,path).  If
        // there aren't any then find all paths in the cache that use the
        // parent.  In either case mark the found paths as needing their
        // property spec stacks blown.
        for (const auto& value : pathsWithSpecChanges) {
            const SdfPath& path        = value.first;
            PathChangeBitmask changes = value.second;

            for (auto cache : caches) {
                Pcp_DidChangeDependents(
                    cache, layer, path, /*processPrimDescendants*/ false, 
                    /*filter*/ false, 
                    [this, &changes, &cache, &layer](const PcpDependency &dep) {
                        // If the changes for this path include something other 
                        // than target changes, they must be spec changes.
                        if (changes & ~(PathChangeTargets | 
                                        PathChangeConnections)) {
                            DidChangeSpecs(cache, dep.indexPath, layer, 
                                           dep.sitePath);
                        }
                        if (changes & PathChangeTargets) {
                            DidChangeTargets(cache, dep.indexPath,
                                PcpCacheChanges::TargetTypeRelationshipTarget);
                        }
                        if (changes & PathChangeConnections) {
                            DidChangeTargets(cache, dep.indexPath,
                                PcpCacheChanges::TargetTypeConnection);
                        }
                    },
                    debugSummary);
            }
        }

        // For every path we've found on this layer that was namespace
        // edited, find all paths in the cache that map to the path and
        // the corresponding new path.  Save these internally for later
        // comparison to edits added through DidChangePaths().
        if (!oldPaths.empty()) {
            SdfPathVector depPaths;

            for (auto cache : caches) {
                _PathEditMap& renameChanges = _GetRenameChanges(cache);

                // Do every path.
                for (size_t i = 0, n = oldPaths.size(); i != n; ++i) {
                    const SdfPath& oldPath = oldPaths[i];
                    const SdfPath& newPath = newPaths[i];
                    // Do every path dependent on the new path.  We might
                    // have an object at the new path and we're replacing
                    // it with the object at the old path.  So we must
                    // act as if we're deleting the object at the new path.
                    if (!newPath.IsEmpty()) {
                        PcpDependencyVector deps =
                            cache->FindSiteDependencies(
                                layer, newPath,
                                PcpDependencyTypeAnyNonVirtual,
                                /* recurseOnSite */ false,
                                /* recurseOnIndex */ false,
                                /* filter */ true
                            );
                        for (const auto &dep: deps) {
                            renameChanges[dep.indexPath] = SdfPath();
                        }
                    }

                    // Do every path dependent on the old path.
                    PcpDependencyVector deps =
                        cache->FindSiteDependencies(
                            layer, oldPath,
                            PcpDependencyTypeAnyNonVirtual,
                            /* recurseOnSite */ false,
                            /* recurseOnIndex */ false,
                            /* filter */ true
                        );
                    for (const auto &dep: deps) {
                        SdfPath newIndexPath;
                        // If this isn't a delete then translate newPath
                        if (!newPath.IsEmpty()) {
                            newIndexPath =
                                dep.mapFunc.MapSourceToTarget(newPath);
                        }
                        renameChanges[dep.indexPath] = newIndexPath;
                        PCP_APPEND_DEBUG("  renameChanges <%s> to <%s>\n",
                             dep.indexPath.GetText(),
                             newIndexPath.GetText());
                    }
                }
            }
        }
    } // end for all layers in changelist map

    // Process layer stack changes.  This will handle both blowing
    // caches (as needed) for the layer stack contents and offsets,
    // as well as analyzing relocation changes in the layer stack.
    for (const auto& entry : layerStackChangesMap) {
        const PcpLayerStackPtr& layerStack = entry.first;
        LayerStackChangeBitmask layerStackChanges = entry.second;

        if (layerStackChanges & LayerStackResolvedPathChange) {
            _DidChangeLayerStackResolvedPath(caches, layerStack, debugSummary);
            if (Pcp_NeedToRecomputeDueToAssetPathChange(layerStack)) {
                layerStackChanges |= LayerStackSignificantChange;
            }
        }

        if (layerStackChanges & LayerStackRelocatesChange) {
            _DidChangeLayerStackRelocations(caches, layerStack, debugSummary);
        }

        _DidChangeLayerStack(
            layerStack,
            layerStackChanges & LayerStackLayersChange,
            layerStackChanges & LayerStackOffsetsChange,
            layerStackChanges & LayerStackSignificantChange);
    }

    if (debugSummary && !debugSummary->empty()) {
        TfDebug::Helper().Msg("PcpChanges::DidChange\n%s",
                              debugSummary->c_str());
    }
}

void 
PcpChanges::DidMuteLayer(
    const PcpCache* cache, 
    const std::string& layerId)
{
    // Change debugging.
    std::string summary;
    std::string* debugSummary = TfDebug::IsEnabled(PCP_CHANGES) ? &summary : 0;

    const SdfLayerRefPtr mutedLayer = 
        _LoadSublayerForChange(cache, layerId, _SublayerRemoved);
    const PcpLayerStackPtrVector& layerStacks = 
        cache->FindAllLayerStacksUsingLayer(mutedLayer);

    PCP_APPEND_DEBUG("  Did mute layer @%s@\n", layerId.c_str());

    if (!layerStacks.empty()) {
        _DidChangeSublayerAndLayerStacks(
            cache, layerStacks, layerId, mutedLayer, _SublayerRemoved, 
            debugSummary);
    }

    if (debugSummary && !debugSummary->empty()) {
        TfDebug::Helper().Msg("PcpChanges::DidMuteLayer\n%s",
                              debugSummary->c_str());
    }
}

void 
PcpChanges::DidUnmuteLayer(
    const PcpCache* cache, 
    const std::string& layerId)
{
    // Change debugging.
    std::string summary;
    std::string* debugSummary = TfDebug::IsEnabled(PCP_CHANGES) ? &summary : 0;

    const SdfLayerRefPtr unmutedLayer = 
        _LoadSublayerForChange(cache, layerId, _SublayerAdded);
    const PcpLayerStackPtrVector& layerStacks = 
        cache->_layerStackCache->FindAllUsingMutedLayer(layerId);

    PCP_APPEND_DEBUG("  Did unmute layer @%s@\n", layerId.c_str());

    if (!layerStacks.empty()) {
        _DidChangeSublayerAndLayerStacks(
            cache, layerStacks, layerId, unmutedLayer, _SublayerAdded, 
            debugSummary);
    }

    if (debugSummary && !debugSummary->empty()) {
        TfDebug::Helper().Msg("PcpChanges::DidUnmuteLayer\n%s",
                              debugSummary->c_str());
    }
}

void
PcpChanges::DidMaybeFixSublayer(
    const PcpCache* cache,
    const SdfLayerHandle& layer,
    const std::string& sublayerPath)
{
    // Change debugging.
    std::string summary;
    std::string* debugSummary = TfDebug::IsEnabled(PCP_CHANGES) ? &summary : 0;

    // See if the sublayer is now readable.  If so mark the layer stacks
    // using the sublayer's parent (and thus the sublayer) as dirty, and also
    // all of the prims in cache that are using any prim from any of those
    // layer stacks.
    const SdfLayerRefPtr sublayer = 
        _LoadSublayerForChange(cache, layer, sublayerPath, _SublayerAdded);
    const PcpLayerStackPtrVector& layerStacks =
        cache->FindAllLayerStacksUsingLayer(layer);

    PCP_APPEND_DEBUG(
        "  Layer @%s@ changed sublayer @%s@\n",
        layer ? layer->GetIdentifier().c_str() : "invalid",
        sublayerPath.c_str());

    _DidChangeSublayerAndLayerStacks(
        cache, layerStacks, sublayerPath, sublayer, _SublayerAdded, 
        debugSummary);

    if (debugSummary && !debugSummary->empty()) {
        TfDebug::Helper().Msg("PcpChanges::DidMaybeFixSublayer\n%s",
                              debugSummary->c_str());
    }
}

void 
PcpChanges::_DidChangeSublayerAndLayerStacks(
    const PcpCache* cache,
    const PcpLayerStackPtrVector& layerStacks,
    const std::string& sublayerPath,
    const SdfLayerHandle& sublayer,
    _SublayerChangeType sublayerChange,
    std::string* debugSummary)
{
    static const bool requiresLayerStackChange        = true;
    static const bool requiresLayerStackOffsetsChange = false;
    bool requiresSignificantChange = false;

    _DidChangeSublayer(cache, layerStacks,
                       sublayerPath, sublayer, sublayerChange,
                       debugSummary, &requiresSignificantChange);

    if (sublayer) {
        // Layer was loaded.  The layer stacks are changed.
        TF_FOR_ALL(layerStack, layerStacks) {
            _DidChangeLayerStack(*layerStack,
                                 requiresLayerStackChange,
                                 requiresLayerStackOffsetsChange,
                                 requiresSignificantChange );
        }
    }
}

void
PcpChanges::DidMaybeFixAsset(
    const PcpCache* cache,
    const PcpSite& site,
    const SdfLayerHandle& srcLayer,
    const std::string& assetPath)
{
    // Get the site's layer stack and make sure it's valid.
    PcpLayerStackPtr layerStack = 
        cache->FindLayerStack(site.layerStackIdentifier);
    if (!layerStack) {
        return;
    }

    // Change debugging.
    std::string summary;
    std::string* debugSummary = TfDebug::IsEnabled(PCP_CHANGES) ? &summary : 0;

    // Load the layer.
    TfErrorMark m;
    SdfLayerRefPtr layer = SdfLayer::FindOrOpenRelativeToLayer(
        srcLayer, assetPath);
    m.Clear();
    
    PCP_APPEND_DEBUG("  Asset @%s@ %s\n",
                     assetPath.c_str(),
                     layer ? (layer->IsEmpty() ? "insignificant"
                                               : "significant")
                           : "invalid");

    if (layer) {
        // Hold layer to avoid reparsing.
        _lifeboat.Retain(layer);

        // Mark prims using site as changed.
        PCP_APPEND_DEBUG(
            "Resync following in @%s@ significantly due to "
            "loading asset used by @%s@<%s>:\n",
            cache->GetLayerStackIdentifier().rootLayer->
                 GetIdentifier().c_str(),
            layerStack->GetIdentifier().rootLayer->
                 GetIdentifier().c_str(), site.path.GetText());
        if (layerStack == cache->GetLayerStack()) {
            PCP_APPEND_DEBUG("    <%s>\n", site.path.GetText());
            DidChangeSignificantly(cache, site.path);
        }
        PcpDependencyVector deps =
            cache->FindSiteDependencies(layerStack, site.path,
                                        PcpDependencyTypeAnyIncludingVirtual,
                                        /* recurseOnSite */ true,
                                        /* recurseOnIndex */ true,
                                        /* filter */ true);
        for(const auto &dep: deps) {
            PCP_APPEND_DEBUG("    <%s>\n", dep.indexPath.GetText());
            DidChangeSignificantly(cache, dep.indexPath);
        }
    }

    if (debugSummary && !debugSummary->empty()) {
        TfDebug::Helper().Msg("PcpChanges::DidMaybeFixAsset\n%s",
                              debugSummary->c_str());
    }
}

void
PcpChanges::DidChangeLayers(const PcpCache* cache)
{
    TF_DEBUG(PCP_CHANGES).Msg("PcpChanges::DidChangeLayers: @%s@\n",
                              cache->GetLayerStackIdentifier().rootLayer->
                                  GetIdentifier().c_str());

    PcpLayerStackChanges& changes = _GetLayerStackChanges(cache);
    if (!changes.didChangeLayers) {
        changes.didChangeLayers       = true;
        changes.didChangeLayerOffsets = false;
    }
}

void
PcpChanges::DidChangeLayerOffsets(const PcpCache* cache)
{
    PcpLayerStackChanges& changes = _GetLayerStackChanges(cache);
    if (!changes.didChangeLayers) {
        changes.didChangeLayerOffsets = true;
    }
}

void
PcpChanges::DidChangeSignificantly(const PcpCache* cache, const SdfPath& path)
{
    _GetCacheChanges(cache).didChangeSignificantly.insert(path);
}

static bool
_NoLongerHasAnySpecs(const PcpPrimIndex& primIndex)
{
    for (const PcpNodeRef &node: primIndex.GetNodeRange()) {
        if (PcpComposeSiteHasPrimSpecs(node)) {
            return false;
        }
    }
    return true;
}

void
PcpChanges::DidChangeSpecs(
    const PcpCache* cache, const SdfPath& path,
    const SdfLayerHandle& changedLayer, const SdfPath& changedPath)
{
    if (path.IsPrimPath()) {
        TF_VERIFY(changedPath.IsPrimOrPrimVariantSelectionPath());
        const bool primWasAdded = changedLayer->HasSpec(changedPath);
        const bool primWasRemoved = !primWasAdded;

        const PcpPrimIndex* primIndex = cache->FindPrimIndex(path);
        if (primIndex && primIndex->HasSpecs()) {
            // If the inert spec removed was the last spec in this prim index,
            // the composed prim no longer exists, so mark it as a significant 
            // change.
            if (primWasRemoved && _NoLongerHasAnySpecs(*primIndex)) {
                DidChangeSignificantly(cache, path);
                return;
            }

            const PcpNodeRef nodeForChangedSpec = 
                primIndex->GetNodeProvidingSpec(changedLayer, changedPath);
            if (nodeForChangedSpec) {
                // If this prim index is instanceable, the addition or removal
                // of an inert spec could affect whether this node is considered
                // instanceable, which would change the prim index's instancing
                // key. Mark it as a significant change if this is the case.
                //
                // Note that we don't handle the case where the node for this
                // spec can't be found, because it should never happen. This is
                // because instanceable nodes cannot be ancestral nodes, and
                // non-ancestral nodes are never culled/removed from the graph,
                // so we should always be able to find them. 
                // 
                // See Pcp_ChildNodeIsInstanceable and _NodeCanBeCulled.
                if (primIndex->IsInstanceable() &&
                    Pcp_ChildNodeInstanceableChanged(nodeForChangedSpec)) {
                    DidChangeSignificantly(cache, path);
                    return;
                }
            }
            else if (primWasAdded) {
                // If we're adding an inert prim spec, it may correspond to a 
                // node that was culled in the prim index at path. If so, we 
                // need to rebuild that index to pick up the new node. We don't 
                // need to rebuild the indexes for namespace descendants because
                // those should not be affected.
                _GetCacheChanges(cache).didChangePrims.insert(path);
                return;
            }
        }
        else {
            // If no prim index was found for this path, we assume that if we're
            // adding an inert spec, it's the first one for this composed prim,
            // so mark it as a significant change.
            if (primWasAdded) {
                DidChangeSignificantly(cache, path);
                return;
            }
        }
    }

    DidChangeSpecStack(cache, path);
}

void 
PcpChanges::DidChangeSpecStack(const PcpCache* cache, const SdfPath& path)
{
    _GetCacheChanges(cache).didChangeSpecs.insert(path);
}

void
PcpChanges::DidChangeTargets(const PcpCache* cache, const SdfPath& path,
                             PcpCacheChanges::TargetType targetType)
{
    _GetCacheChanges(cache).didChangeTargets[path] |= targetType;
}

void
PcpChanges::DidChangeRelocates(const PcpCache* cache, const SdfPath& path)
{
    // XXX For now we resync the prim entirely.  This is both because
    // we do not yet have a way to incrementally update the mappings,
    // as well as to ensure that we provide a change entry that will
    // cause Csd to pull on the cache and keep its contents alive.
    _GetCacheChanges(cache).didChangeSignificantly.insert(path);
}

void
PcpChanges::DidChangePaths(
    const PcpCache* cache,
    const SdfPath& oldPath,
    const SdfPath& newPath)
{
    TF_DEBUG(PCP_CHANGES).Msg("PcpChanges::DidChangePaths: @%s@<%s> to <%s>\n",
                              cache->GetLayerStackIdentifier().rootLayer->
                                  GetIdentifier().c_str(),
                              oldPath.GetText(), newPath.GetText());

    // Changes are ordered. A chain of A -> B; B -> C is different than a 
    // parallel move B -> C; A -> B
    _GetCacheChanges(cache).didChangePath.emplace_back(oldPath, newPath);
}

void
PcpChanges::DidDestroyCache(const PcpCache* cache)
{
    _cacheChanges.erase(const_cast<PcpCache*>(cache));
    _renameChanges.erase(const_cast<PcpCache*>(cache));

    // Note that a layer stack in _layerStackChanges may be expired.  We
    // just leave it there and let clients and Apply() check for expired
    // layer stacks.
}

void
PcpChanges::Swap(PcpChanges& other)
{
    std::swap(_layerStackChanges, other._layerStackChanges);
    std::swap(_cacheChanges, other._cacheChanges);
    std::swap(_renameChanges, other._renameChanges);
    _lifeboat.Swap(other._lifeboat);
}

bool
PcpChanges::IsEmpty() const
{
    return _layerStackChanges.empty() &&
           _cacheChanges.empty()      &&
           _renameChanges.empty();
}

const PcpChanges::LayerStackChanges&
PcpChanges::GetLayerStackChanges() const
{
    return _layerStackChanges;
}

const PcpChanges::CacheChanges&
PcpChanges::GetCacheChanges() const
{
    // NOTE: This is potentially expensive even if we've already done
    //       it.  In the expected use pattern we only call this method
    //       once, so it shouldn't be a problem.
    _Optimize();

    return _cacheChanges;
}

const PcpLifeboat&
PcpChanges::GetLifeboat() const
{
    return _lifeboat;
}

void
PcpChanges::Apply() const
{
    // NOTE: This is potentially expensive even if we've already done
    //       it.  In the expected use pattern we only call this method
    //       once, so it shouldn't be a problem.
    _Optimize();

    // Apply layer changes first.
    TF_FOR_ALL(i, _layerStackChanges) {
        if (i->first) {
            i->first->Apply(i->second, &_lifeboat);
        }
    }

    // Now apply cache changes.
    TF_FOR_ALL(i, _cacheChanges) {
        i->first->Apply(i->second, &_lifeboat);
    }
}

PcpLayerStackChanges&
PcpChanges::_GetLayerStackChanges(const PcpCache* cache)
{
    return _layerStackChanges[cache->GetLayerStack()];
}

PcpLayerStackChanges&
PcpChanges::_GetLayerStackChanges(const PcpLayerStackPtr& layerStack)
{
    return _layerStackChanges[layerStack];
}

PcpCacheChanges&
PcpChanges::_GetCacheChanges(const PcpCache* cache)
{
    return _cacheChanges[const_cast<PcpCache*>(cache)];
}

PcpChanges::_PathEditMap&
PcpChanges::_GetRenameChanges(const PcpCache* cache)
{
    return _renameChanges[const_cast<PcpCache*>(cache)];
}

void
PcpChanges::_Optimize() const
{
    const_cast<PcpChanges*>(this)->_Optimize();
}

void
PcpChanges::_Optimize()
{
    TF_FOR_ALL(i, _renameChanges) {
        _OptimizePathChanges(i->first, &_cacheChanges[i->first], &i->second);
    }

    // This must be called after _OptimizePathChanges().
    TF_FOR_ALL(i, _cacheChanges) {
        _Optimize(&i->second);
    }
}

void
PcpChanges::_Optimize(PcpCacheChanges* changes)
{
    // Subsume changes implied by ancestors.
    Pcp_SubsumeDescendants(&changes->didChangeSignificantly);

    // Subsume changes implied by prim graph changes.
    TF_FOR_ALL(i, changes->didChangeSignificantly) {
        Pcp_SubsumeDescendants(&changes->didChangePrims, *i);
        Pcp_SubsumeDescendants(&changes->didChangeSpecs, *i);
        Pcp_SubsumeDescendants(&changes->_didChangeSpecsInternal, *i);
    }

    // Subsume spec changes for prims whose indexes will be rebuilt.
    TF_FOR_ALL(i, changes->didChangePrims) {
        changes->didChangeSpecs.erase(*i);
        changes->_didChangeSpecsInternal.erase(*i);
    }

    // Subsume spec changes that don't change the contents of the stack
    // changes against those that may change the contents.
    TF_FOR_ALL(i, changes->didChangeSpecs) {
        changes->_didChangeSpecsInternal.erase(*i);
    }

    // XXX: Do we subsume name changes?
}

void
PcpChanges::_OptimizePathChanges(
    const PcpCache* cache,
    PcpCacheChanges* changes,
    const _PathEditMap* pathChanges)
{
    // XXX: DidChangePaths handles rename chains. I.e. A renamed to B
    //      then renamed to C. pathChanges is a map but we may need to handle 
    //      one oldPath appearing multiple times in didChangePath, e.g. 
    //      A -> B -> C and D -> B -> E, where B appears in two chains.

    // Copy the path changes and discard any that are also in 
    // changes->didChangePath.
    _PathEditMap sdOnly(*pathChanges);
    for (const auto &pathPair : changes->didChangePath) {
        auto it = sdOnly.find(pathPair.first);
        // Note that we check for exact matches of mapping oldPath to newPath.
        if (it != sdOnly.end() && it->second == pathPair.second) {
            sdOnly.erase(it);
        }
    }

    std::string summary;
    std::string* debugSummary = TfDebug::IsEnabled(PCP_CHANGES) ? &summary : 0;

    // sdOnly now has the path changes that Sd told us about but
    // DidChangePaths() did not.  We must assume the worst.
    for (const auto& change : sdOnly) {
        const SdfPath& oldPath = change.first;
        const SdfPath& newPath = change.second;

        PCP_APPEND_DEBUG("  Sd only path change @%s@<%s> to <%s>\n",
                         cache->GetLayerStackIdentifier().rootLayer->
                             GetIdentifier().c_str(),
                         oldPath.GetText(), newPath.GetText());
        changes->didChangeSignificantly.insert(oldPath);
        if (!newPath.IsEmpty()) {
            changes->didChangeSignificantly.insert(newPath);
        }
    }

    if (debugSummary && !debugSummary->empty()) {
        TfDebug::Helper().Msg("PcpChanges::_Optimize:\n%s",
                              debugSummary->c_str());
    }
}

SdfLayerRefPtr 
PcpChanges::_LoadSublayerForChange(
    const PcpCache* cache,
    const std::string& sublayerPath,
    _SublayerChangeType sublayerChange) const
{
    // Bind the resolver context.
    const ArResolverContextBinder binder(
        cache->GetLayerStackIdentifier().pathResolverContext);

    // Load the layer.
    SdfLayerRefPtr sublayer;

    const SdfLayer::FileFormatArguments sublayerArgs = 
        Pcp_GetArgumentsForFileFormatTarget(
            sublayerPath, cache->GetFileFormatTarget());

    if (sublayerChange == _SublayerAdded) {
        sublayer = SdfLayer::FindOrOpen(sublayerPath, sublayerArgs);
    }
    else {
        sublayer = SdfLayer::Find(sublayerPath, sublayerArgs);
    }

    return sublayer;
}

SdfLayerRefPtr
PcpChanges::_LoadSublayerForChange(
    const PcpCache* cache,
    const SdfLayerHandle& layer,
    const std::string& sublayerPath,
    _SublayerChangeType sublayerChange) const
{
    if (!layer) {
        return SdfLayerRefPtr();
    }

    // Bind the resolver context.
    const ArResolverContextBinder binder(
        cache->GetLayerStackIdentifier().pathResolverContext);

    // Load the layer.
    SdfLayerRefPtr sublayer;

    const SdfLayer::FileFormatArguments sublayerArgs = 
        Pcp_GetArgumentsForFileFormatTarget(
            sublayerPath, cache->GetFileFormatTarget());

    // Note the possible conversions from SdfLayerHandle to SdfLayerRefPtr below.
    if (SdfLayer::IsAnonymousLayerIdentifier(sublayerPath)) {
        sublayer = SdfLayer::Find(sublayerPath, sublayerArgs);
    }
    else {
        // Don't bother trying to open a sublayer if we're removing it;
        // either it's already opened in the system and we'll find it, or
        // it's invalid, which we'll deal with below.
        if (sublayerChange == _SublayerAdded) {
            TfErrorMark m;
            sublayer = SdfLayer::FindOrOpenRelativeToLayer(
                layer, sublayerPath, sublayerArgs);
            m.Clear();
        }
        else {
            sublayer = SdfLayer::FindRelativeToLayer(
                layer, sublayerPath, sublayerArgs);
        }
    }
    
    return sublayer;
}

void
PcpChanges::_DidChangeSublayer(
    const PcpCache* cache,
    const PcpLayerStackPtrVector& layerStacks,
    const std::string& sublayerPath,
    const SdfLayerHandle& sublayer,
    _SublayerChangeType sublayerChange,
    std::string* debugSummary,
    bool *significant)
{
    *significant = (sublayer && !sublayer->IsEmpty());

    PCP_APPEND_DEBUG("  %s sublayer @%s@ %s\n",
                     sublayer ? (*significant ? "significant"
                                              : "insignificant")
                              : "invalid",
                     sublayerPath.c_str(),
                     sublayerChange == _SublayerAdded ? "added" : "removed");

    if (!sublayer || (!*significant && cache->IsUsd())) {
        // If the added or removed sublayer is invalid, or if it is
        // insignificant and this cache is a USD cache, then it has no effect on
        // composed results so we don't need to register any changes.
        return;
    }

    // Keep the layer alive to avoid reparsing.
    _lifeboat.Retain(sublayer);

    // Register change entries for affected paths.
    //
    // For significant sublayer changes, the sublayer may have introduced
    // new prims with new arcs, requiring prim and property indexes to be
    // recomputed. So, register significant changes for every prim path
    // in the cache that uses any path in any of the layer stacks that
    // included layer.  Only bother doing this for prims, since the
    // properties will be implicitly invalidated by significant
    // prim resyncs.
    //
    // For insignificant sublayer changes, the only prim that's really 
    // affected is the pseudo-root. However, we still need to rebuild the 
    // prim stacks for every prim that uses an affected layer stack. This
    // is because PcpPrimIndex's prim stack stores indices into the layer
    // stack that may need to be adjusted due to the addition or removal of
    // a layer from that stack.
    //
    // We rely on the caller to provide the affected layer stacks for
    // us because some changes introduce new dependencies that wouldn't
    // have been registered yet using the normal means -- such as unmuting
    // a sublayer.

    bool anyFound = false;
    TF_FOR_ALL(layerStack, layerStacks) {
        PcpDependencyVector deps = cache->FindSiteDependencies(
            *layerStack,
            SdfPath::AbsoluteRootPath(), 
            PcpDependencyTypeAnyIncludingVirtual,
            /* recurseOnSite */ true,
            /* recurseOnIndex */ true,
            /* filter */ true);
        for (const auto &dep: deps) {
            if (!dep.indexPath.IsAbsoluteRootOrPrimPath()) {
                // Filter to only prims; see comment above re: properties.
                continue;
            }
            if (!anyFound) {
                PCP_APPEND_DEBUG(
                    "  %s following in @%s@ due to "
                    "%s reload in sublayer @%s@:\n",
                    *significant ? "Resync" : "Spec changes",
                    cache->GetLayerStackIdentifier().rootLayer->
                         GetIdentifier().c_str(),
                    *significant ? "significant" : "insignificant",
                    sublayer->GetIdentifier().c_str());
                anyFound = true;
            }
            PCP_APPEND_DEBUG("    <%s>\n", dep.indexPath.GetText());
            if (*significant) {
                DidChangeSignificantly(cache, dep.indexPath);
            } else {
                _DidChangeSpecStackInternal(cache, dep.indexPath);
            }
        }
    }
}

void
PcpChanges::_DidChangeLayerStack(
    const PcpLayerStackPtr& layerStack,
    bool requiresLayerStackChange,
    bool requiresLayerStackOffsetsChange,
    bool requiresSignificantChange)
{
    PcpLayerStackChanges& changes = _GetLayerStackChanges(layerStack);
    changes.didChangeLayers        |= requiresLayerStackChange;
    changes.didChangeLayerOffsets  |= requiresLayerStackOffsetsChange;
    changes.didChangeSignificantly |= requiresSignificantChange;

    // didChangeLayers subsumes didChangeLayerOffsets.
    if (changes.didChangeLayers) {
        changes.didChangeLayerOffsets = false;
    }
}

static void
_DeterminePathsAffectedByRelocationChanges( const SdfRelocatesMap & oldMap,
                                            const SdfRelocatesMap & newMap,
                                            SdfPathSet *affectedPaths )
{
    TF_FOR_ALL(path, oldMap) {
        SdfRelocatesMap::const_iterator i = newMap.find(path->first);
        if (i == newMap.end() || i->second != path->second) {
            // This entry in oldMap does not exist in newMap, or
            // newMap relocates this to a different path.
            // Record the affected paths.
            affectedPaths->insert(path->first);
            affectedPaths->insert(path->second);
            if (i != newMap.end()) {
                affectedPaths->insert(i->second);
            }
        }
    }
    TF_FOR_ALL(path, newMap) {
        SdfRelocatesMap::const_iterator i = oldMap.find(path->first);
        if (i == oldMap.end() || i->second != path->second) {
            // This entry in newMap does not exist in oldMap, or
            // oldMap relocated this to a different path.
            // Record the affected paths.
            affectedPaths->insert(path->first);
            affectedPaths->insert(path->second);
            if (i != oldMap.end()) {
                affectedPaths->insert(i->second);
            }
        }
    }
}

// Handle changes to relocations.  This requires:
// 1. rebuilding the composed relocation tables in layer stacks
// 2. blowing PrimIndex caches affected by relocations
// 3. rebuilding MapFunction values that consumed those relocations
void
PcpChanges::_DidChangeLayerStackRelocations(
    const TfSpan<const PcpCache*>& caches,
    const PcpLayerStackPtr & layerStack,
    std::string* debugSummary)
{
    PcpLayerStackChanges& changes = _GetLayerStackChanges(layerStack);

    if (changes.didChangeRelocates) {
        // There might be multiple relocation changes in a given
        // layer stack, but we only need to process them once.
        return;
    }

    changes.didChangeRelocates = true;

    // Rebuild this layer stack's composed relocations.
    // Store the result in the PcpLayerStackChanges so they can
    // be committed when the changes are applied.
    Pcp_ComputeRelocationsForLayerStack(
        layerStack->GetLayers(),
        &changes.newRelocatesSourceToTarget,
        &changes.newRelocatesTargetToSource,
        &changes.newIncrementalRelocatesSourceToTarget,
        &changes.newIncrementalRelocatesTargetToSource,
        &changes.newRelocatesPrimPaths);

    // Compare the old and new relocations to determine which
    // paths (in this layer stack) are affected.
    _DeterminePathsAffectedByRelocationChanges(
        layerStack->GetRelocatesSourceToTarget(),
        changes.newRelocatesSourceToTarget,
        &changes.pathsAffectedByRelocationChanges);

    // Resync affected prims.
    // Use dependencies to find affected caches.
    if (!changes.pathsAffectedByRelocationChanges.empty()) {
        PCP_APPEND_DEBUG("  Relocation change in %s affects:\n",
                         TfStringify(layerStack).c_str());
    }
    for (const PcpCache* cache: caches) {
        // Find the equivalent layer stack in this cache.
        PcpLayerStackPtr equivLayerStack =
            cache->FindLayerStack(layerStack->GetIdentifier());
        if (!equivLayerStack) {
            continue;
        }

        SdfPathSet depPathSet;
        for (const SdfPath& path : changes.pathsAffectedByRelocationChanges) {
            PCP_APPEND_DEBUG("    <%s>\n", path.GetText());

            PcpDependencyVector deps =
                cache->FindSiteDependencies(
                    equivLayerStack, path,
                    PcpDependencyTypeAnyIncludingVirtual,
                    /* recurseOnSite */ true,
                    /* recurseOnIndex */ true,
                    /* filterForExistingCachesOnly */ false);
            for (const auto &dep: deps) {
                depPathSet.insert(dep.indexPath);
            }
        }

        if (!depPathSet.empty()) {
            PCP_APPEND_DEBUG("  and dependent paths in %s\n",
                             TfStringify(layerStack).c_str());
        }
        for (const SdfPath& depPath : depPathSet) {
            PCP_APPEND_DEBUG("      <%s>\n", depPath.GetText());
            DidChangeSignificantly(cache, depPath);
        }
    }
}

void 
PcpChanges::_DidChangeLayerStackResolvedPath(
    const TfSpan<const PcpCache*>& caches,
    const PcpLayerStackPtr& layerStack,
    std::string* debugSummary)
{
    const ArResolverContextBinder binder(
        layerStack->GetIdentifier().pathResolverContext);

    for (const PcpCache* cache : caches) {
        PcpDependencyVector deps = 
            cache->FindSiteDependencies(
                layerStack, SdfPath::AbsoluteRootPath(),
                PcpDependencyTypeAnyIncludingVirtual,
                /* recurseOnSite */ true,
                /* recurseOnIndex */ false,
                /* filterForExisting */ true);

        auto noResyncNeeded = [cache](const PcpDependency& dep) {
            if (!dep.indexPath.IsPrimPath()) { 
                return true; 
            }
            const PcpPrimIndex* primIndex = cache->FindPrimIndex(dep.indexPath);
            return (TF_VERIFY(primIndex) && 
                    !Pcp_NeedToRecomputeDueToAssetPathChange(*primIndex));
        };

        deps.erase(
            std::remove_if(deps.begin(), deps.end(), noResyncNeeded),
            deps.end());
        if (deps.empty()) {
            continue;
        }

        PCP_APPEND_DEBUG(
            "   Resync following in @%s@ significant due to layer "
            "resolved path change:\n",
            cache->GetLayerStackIdentifier().rootLayer->
                GetIdentifier().c_str());

        for (const PcpDependency& dep : deps) {
            PCP_APPEND_DEBUG("    <%s>\n", dep.indexPath.GetText());
            DidChangeSignificantly(cache, dep.indexPath);
        }
    }
}

void 
PcpChanges::_DidChangeSpecStackInternal(
    const PcpCache* cache, const SdfPath& path)
{
    _GetCacheChanges(cache)._didChangeSpecsInternal.insert(path);
}

PXR_NAMESPACE_CLOSE_SCOPE