﻿//
//  ItemHighlightScriptingInterface.cpp
//  interface/src/scripting
//
//  Created by Zach Fox on 2017-08-22.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ItemHighlightScriptingInterface.h"
#include "Application.h"

ItemHighlightScriptingInterface::ItemHighlightScriptingInterface(AbstractViewStateInterface* viewState) {
    _viewState = viewState;
}

//
// START HANDLING ENTITIES
//
render::ItemID getItemIDFromEntityID(const EntityItemID& entityID) {
    auto entityTree = qApp->getEntities()->getTree();
    entityTree->withReadLock([&] {
        auto entityItem = entityTree->findEntityByEntityItemID(entityID);
        if (entityItem != NULL) {
            auto renderableInterface = entityItem->getRenderableInterface();
            if (renderableInterface != NULL) {
                return renderableInterface->getMetaRenderItemID();
            }
        }
        return render::Item::INVALID_ITEM_ID;
    });
    return render::Item::INVALID_ITEM_ID;
}
bool ItemHighlightScriptingInterface::addToHighlightedItemsList(const EntityItemID& entityID) {
    render::ItemID itemID = getItemIDFromEntityID(entityID);
    if (itemID != render::Item::INVALID_ITEM_ID) {
        return addToHighlightedItemsList(itemID);
    }
    return false;
}
bool ItemHighlightScriptingInterface::removeFromHighlightedItemsList(const EntityItemID& entityID) {
    render::ItemID itemID = getItemIDFromEntityID(entityID);
    if (itemID != render::Item::INVALID_ITEM_ID) {
        return removeFromHighlightedItemsList(itemID);
    }
    return false;
}
//
// END HANDLING ENTITIES
//

//
// START HANDLING OVERLAYS
//
render::ItemID getItemIDFromOverlayID(const OverlayID& overlayID) {
    auto& overlays = qApp->getOverlays();
    auto overlay = overlays.getOverlay(overlayID);
    if (overlay != NULL) {
        auto itemID = overlay->getRenderItemID();
        if (itemID != render::Item::INVALID_ITEM_ID) {
            return overlay->getRenderItemID();
        }
    }
    return render::Item::INVALID_ITEM_ID;
}
bool ItemHighlightScriptingInterface::addToHighlightedItemsList(const OverlayID& overlayID) {
    render::ItemID itemID = getItemIDFromOverlayID(overlayID);
    if (itemID != render::Item::INVALID_ITEM_ID) {
        return addToHighlightedItemsList(itemID);
    }
    return false;
}
bool ItemHighlightScriptingInterface::removeFromHighlightedItemsList(const OverlayID& overlayID) {
    render::ItemID itemID = getItemIDFromOverlayID(overlayID);
    if (itemID != render::Item::INVALID_ITEM_ID) {
        return removeFromHighlightedItemsList(itemID);
    }
    return false;
}
//
// END HANDLING OVERLAYS
//

//
// START HANDLING GENERIC ITEMS
//
bool ItemHighlightScriptingInterface::addToHighlightedItemsList(render::ItemID idToAdd) {
    _highlightedItemsList.push_back(idToAdd); // TODO: Ensure thread safety
    updateRendererHighlightList();
    return true;
}
bool ItemHighlightScriptingInterface::removeFromHighlightedItemsList(render::ItemID idToRemove) {
    auto itr = std::find(_highlightedItemsList.begin(), _highlightedItemsList.end(), idToRemove); // TODO: Ensure thread safety
    if (itr == _highlightedItemsList.end()) {
        return false;
    } else {
        _highlightedItemsList.erase(itr);
        updateRendererHighlightList();
        return true;
    }
}
//
// END HANDLING GENERIC ITEMS
//

void ItemHighlightScriptingInterface::updateRendererHighlightList() {
    auto scene = _viewState->getMain3DScene();
    if (scene) {
        render::Transaction transaction;

        render::Selection selection("Highlight", _highlightedItemsList);
        transaction.resetSelection(selection);

        scene->enqueueTransaction(transaction);
    } else {
        qWarning() << "ItemHighlightScriptingInterface::updateRendererHighlightList(), Unexpected null scene, possibly during application shutdown";
    }
}
