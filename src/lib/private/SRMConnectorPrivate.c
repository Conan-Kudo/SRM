#include <private/SRMConnectorPrivate.h>
#include <private/SRMConnectorModePrivate.h>
#include <private/SRMDevicePrivate.h>
#include <private/SRMEncoderPrivate.h>
#include <private/SRMCrtcPrivate.h>
#include <private/SRMPlanePrivate.h>
#include <private/SRMCorePrivate.h>

#include <private/modes/SRMRenderModeCommon.h>
#include <private/modes/SRMRenderModeItself.h>

#include <SRMLog.h>
#include <SRMList.h>

#include <stdio.h>
#include <xf86drmMode.h>
#include <libdisplay-info/edid.h>
#include <libdisplay-info/info.h>

#include <string.h>
#include <errno.h>
#include <stdlib.h>

SRMConnector *srmConnectorCreate(SRMDevice *device, UInt32 id)
{
    SRMConnector *connector = calloc(1, sizeof(SRMConnector));
    connector->id = id;
    connector->device = device;
    connector->state = SRM_CONNECTOR_STATE_UNINITIALIZED;

    srmConnectorUpdateProperties(connector);

    // This is called after its device is added to the core devices list
    // srmConnectorUpdateNames(connector);

    srmConnectorUpdateEncoders(connector);
    srmConnectorUpdateModes(connector);

    return connector;
}

// Recursively find a free id to add at the end of the name (e.g HDMI-A-0)
// so that connectors of the same type have unique names
UInt32 srmConnectorGetFreeNameID(SRMConnector *connector, UInt32 id)
{
    SRMListForeach (item1, connector->device->core->devices)
    {
        SRMDevice *device = srmListItemGetData(item1);

        SRMListForeach (item2, device->connectors)
        {
            SRMConnector *otherConnector = srmListItemGetData(item2);

            if (otherConnector == connector)
                continue;

            // Ups ID already used by another connector, try with the next value
            if (otherConnector->type == connector->type && otherConnector->nameID == id)
            {
                id++;
                return srmConnectorGetFreeNameID(connector, id);
            }
        }
    }

    // Got a free ID
    return id;
}

UInt8 srmConnectorUpdateProperties(SRMConnector *connector)
{
    drmModeConnector *connectorRes = drmModeGetConnector(connector->device->fd, connector->id);

    if (!connectorRes)
    {
        SRMError("Could not get device %s connector %d resources.", connector->device->name, connector->id);
        return 0;
    }

    connector->mmHeight = connectorRes->mmHeight;
    connector->mmWidth = connectorRes->mmWidth;
    connector->connected = connectorRes->connection == DRM_MODE_CONNECTED;
    connector->type = connectorRes->connector_type;

    drmModeFreeConnector(connectorRes);

    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(connector->device->fd, connector->id, DRM_MODE_OBJECT_CONNECTOR);

    if (!props)
    {
        SRMError("Could not get device %s connector %d properties.", connector->device->name, connector->id);
        return 0;
    }

    memset(&connector->propIDs, 0, sizeof(struct SRMConnectorPropIDs));

    for (UInt32 i = 0; i < props->count_props; i++)
    {
        drmModePropertyPtr prop = drmModeGetProperty(connector->device->fd, props->props[i]);

        if (!prop)
        {
            SRMWarning("Could not get property %d of connector %d.", props->props[i], connector->id);
            continue;
        }

        if (strcmp(prop->name, "CRTC_ID") == 0)
            connector->propIDs.CRTC_ID = prop->prop_id;
        else if (strcmp(prop->name, "DPMS") == 0)
            connector->propIDs.DPMS = prop->prop_id;
        else if (strcmp(prop->name, "EDID") == 0)
            connector->propIDs.EDID = prop->prop_id;
        else if (strcmp(prop->name, "PATH") == 0)
            connector->propIDs.PATH = prop->prop_id;
        else if (strcmp(prop->name, "link-status") == 0)
            connector->propIDs.link_status = prop->prop_id;
        else if (strcmp(prop->name, "non-desktop") == 0)
            connector->propIDs.non_desktop = prop->prop_id;
        else if (strcmp(prop->name, "panel orientation") == 0)
            connector->propIDs.panel_orientation = prop->prop_id;
        else if (strcmp(prop->name, "subconnector") == 0)
            connector->propIDs.subconnector = prop->prop_id;
        else if (strcmp(prop->name, "vrr_capable") == 0)
            connector->propIDs.vrr_capable = prop->prop_id;

        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);

    return 1;
}

UInt8 srmConnectorUpdateNames(SRMConnector *connector)
{  
    srmConnectorDestroyNames(connector);

    // Search a free name ID for the connector type
    connector->nameID = srmConnectorGetFreeNameID(connector, 0);

    // Set name
    char name[64];
    memset(name, 0, sizeof(name));
    sprintf(name, "%s-%d", srmGetConnectorTypeString(connector->type), connector->nameID);
    Int32 nameLen = strlen(name) + 1;
    connector->name = malloc(nameLen);
    memcpy(connector->name, name, nameLen);

    if (!connector->connected)
        return 0;

    drmModeConnector *connectorRes = drmModeGetConnector(connector->device->fd, connector->id);

    if (!connectorRes)
    {
        SRMError("Could not get device %s connector %d resources.", connector->device->name, connector->id);
        return 0;
    }

    drmModePropertyBlobPtr blob = NULL;

    for (int i = 0; i < connectorRes->count_props; i++)
    {
        drmModePropertyPtr prop = drmModeGetProperty(connector->device->fd, connectorRes->props[i]);

        if (prop && (strcmp(prop->name, "EDID") == 0))
        {
            blob = drmModeGetPropertyBlob(connector->device->fd, connectorRes->prop_values[i]);
            drmModeFreeProperty(prop);
            break;
        }

        drmModeFreeProperty(prop);
    }

    if (!blob)
    {
        SRMError("Error getting device %s EDID property blob for connector %d: %s", connector->device->name, connector->id, strerror(errno));
        drmModeFreeConnector(connectorRes);
        return 0;
    }

    struct di_info *info = di_info_parse_edid(blob->data, blob->length);

    if (!info)
    {
        SRMError("Failed to parse device %s EDID of connector %d: %s", connector->device->name, connector->id, strerror(errno));
        drmModeFreePropertyBlob(blob);
        drmModeFreeConnector(connectorRes);
        return 0;
    }

    int len = strlen(di_info_get_make(info));

    if (len > 0)
    {
        connector->manufacturer = malloc(len+1);
        memcpy(connector->manufacturer, di_info_get_make(info), len+1);
    }

    len = strlen(di_info_get_model(info));

    if (len > 0)
    {
        connector->model = malloc(len+1);
        memcpy(connector->model, di_info_get_model(info), len+1);
    }

    di_info_destroy(info);

    drmModeFreePropertyBlob(blob);
    drmModeFreeConnector(connectorRes);

    return 1;
}

void srmConnectorDestroyNames(SRMConnector *connector)
{
    if (connector->name)
    {
        free(connector->name);
        connector->name = NULL;
    }

    if (connector->manufacturer)
    {
        free(connector->manufacturer);
        connector->manufacturer = NULL;
    }

    if (connector->model)
    {
        free(connector->model);
        connector->model = NULL;
    }
}

UInt8 srmConnectorUpdateEncoders(SRMConnector *connector)
{
    srmConnectorDestroyEncoders(connector);
    connector->encoders = srmListCreate();

    drmModeConnector *connectorRes = drmModeGetConnector(connector->device->fd, connector->id);

    if (!connectorRes)
    {
        SRMError("Could not get device %s connector %d resources.", connector->device->name, connector->id);
        return 0;
    }

    for (int i = 0; i < connectorRes->count_encoders; i++)
    {
        SRMListForeach(item, connector->device->encoders)
        {
            SRMEncoder *encoder = srmListItemGetData(item);

            if (encoder->id == connectorRes->encoders[i])
                srmListAppendData(connector->encoders, encoder);
        }
    }

    drmModeFreeConnector(connectorRes);

    return 1;
}

void srmConnectorDestroyEncoders(SRMConnector *connector)
{
    if (connector->encoders)
    {
        srmListDestoy(connector->encoders);
        connector->encoders = NULL;
    }
}

UInt8 srmConnectorUpdateModes(SRMConnector *connector)
{
    srmConnectorDestroyModes(connector);
    connector->modes = srmListCreate();

    drmModeConnector *connectorRes = drmModeGetConnector(connector->device->fd, connector->id);

    if (!connectorRes)
    {
        SRMError("Could not get device %s connector %d resources.", connector->device->name, connector->id);
        return 0;
    }

    for (int i = 0; i < connectorRes->count_modes; i++)
    {
        SRMConnectorMode *connectorMode = srmConnectorModeCreate(connector, (void*)&connectorRes->modes[i]);

        if (connectorMode)
            connectorMode->connectorLink = srmListAppendData(connector->modes, connectorMode);

    }

    connector->preferredMode = srmConnectorFindPreferredMode(connector);

    // Set the preferred as default
    connector->currentMode = connector->preferredMode;

    drmModeFreeConnector(connectorRes);

    return 1;
}

void srmConnectorDestroyModes(SRMConnector *connector)
{
    if (connector->modes)
    {
        while (!srmListIsEmpty(connector->modes))
        {
            SRMConnectorMode *mode = srmListItemGetData(srmListGetBack(connector->modes));
            srmConnectorModeDestroy(mode);
        }

        srmListDestoy(connector->modes);
        connector->modes = NULL;
    }
}

SRMConnectorMode *srmConnectorFindPreferredMode(SRMConnector *connector)
{
    SRMConnectorMode *preferredMode = NULL;

    Int32 greatestSize = -1;

    SRMListForeach(item, connector->modes)
    {
        SRMConnectorMode *connectorMode = srmListItemGetData(item);

        if (connectorMode->info.type & DRM_MODE_TYPE_PREFERRED)
        {
            preferredMode = connectorMode;
            break;
        }

        // If no mode has the preferred flag, look for the one with greatest dimensions
        Int32 currentSize = connectorMode->info.hdisplay * connectorMode->info.vdisplay;

        if (currentSize > greatestSize)
        {
            greatestSize = currentSize;
            preferredMode = connectorMode;
        }
    }

    return preferredMode;
}

// Find a valid (encoder,crtc and primary plane) trio and a cursor plane if avaliable
// Returns 1 if a valid trio is found and 0 otherwise
UInt8 srmConnectorGetBestConfiguration(SRMConnector *connector, SRMEncoder **bestEncoder, SRMCrtc **bestCrtc, SRMPlane **bestPrimaryPlane, SRMPlane **bestCursorPlane)
{
    int bestScore = 0;
    *bestEncoder = NULL;
    *bestCrtc = NULL;
    *bestPrimaryPlane = NULL;
    *bestCursorPlane = NULL;

    SRMListForeach (item1, connector->encoders)
    {
        SRMEncoder *encoder = srmListItemGetData(item1);

        SRMListForeach (item2, encoder->crtcs)
        {
            SRMCrtc *crtc = srmListItemGetData(item2);

            // Check if already used by other connector
            if (crtc->currentConnector)
                continue;

            int score = 0;
            SRMPlane *primaryPlane = NULL;
            SRMPlane *cursorPlane = NULL;

            // Check if has a cursor plane
            SRMListForeach (item3, connector->device->planes)
            {
                SRMPlane *plane = srmListItemGetData(item3);

                if (plane->type == SRM_PLANE_TYPE_OVERLAY)
                    continue;

                SRMListForeach (item4, plane->crtcs)
                {
                    SRMCrtc *planeCrtc = srmListItemGetData(item4);

                    // Check if already used by other connector
                    if (planeCrtc->currentConnector)
                        continue;

                    if (planeCrtc->id == crtc->id)
                    {
                        if (plane->type == SRM_PLANE_TYPE_PRIMARY)
                        {
                            primaryPlane = plane;
                            break;
                        }
                        else if (plane->type == SRM_PLANE_TYPE_CURSOR)
                        {
                            cursorPlane = plane;
                            break;
                        }
                    }
                }
            }

            // Can not render if no primary plane
            if (!primaryPlane)
                continue;

            score += 100;

            if (cursorPlane)
                score += 50;

            if (score > bestScore)
            {
                bestScore = score;
                *bestEncoder = encoder;
                *bestCrtc = crtc;
                *bestPrimaryPlane = primaryPlane;
                *bestCursorPlane = cursorPlane;
            }
        }
    }

    return *bestEncoder && *bestCrtc && *bestPrimaryPlane;
}

void srmConnectorDestroy(SRMConnector *connector)
{
    srmConnectorUninitialize(connector);
    srmConnectorDestroyNames(connector);
    srmConnectorDestroyEncoders(connector);
    srmConnectorDestroyModes(connector);
    srmListRemoveItem(connector->device->connectors, connector->deviceLink);
    free(connector);
}

void *srmConnectorRenderThread(void *conn)
{
    SRMConnector *connector = conn;

    if (pthread_mutex_init(&connector->repaintMutex, NULL))
    {
        SRMError("Could not create render mutex for device %s connector %d.",
                 connector->device->name,
                 connector->id);
        goto fail;
    }

    if (pthread_cond_init(&connector->repaintCond, NULL))
    {
        SRMError("Could not create render cond for device %s connector %d.",
                 connector->device->name,
                 connector->id);
        goto fail;
    }

    connector->pendingPageFlip = 0;

    connector->drmEventCtx.version = DRM_EVENT_CONTEXT_VERSION,
    connector->drmEventCtx.vblank_handler = NULL,
    connector->drmEventCtx.page_flip_handler = &srmRenderModeCommonPageFlipHandler,
    connector->drmEventCtx.page_flip_handler2 = NULL;
    connector->drmEventCtx.sequence_handler = NULL;

    srmRenderModeCommonCreateCursor(connector);

    if (srmDeviceGetRenderMode(connector->device) == SRM_RENDER_MODE_ITSELF)
        srmRenderModeItselfSetInterface(connector);
    else
        goto fail;

    if (!connector->renderInterface.initialize(connector))
        goto fail;

    connector->renderInitResult = 1;

    // Render loop
    while (1)
    {
        if (!srmRenderModeCommonWaitRepaintRequest(connector))
            break;

        if (connector->repaintRequested)
        {
            if (connector->state == SRM_CONNECTOR_STATE_INITIALIZED)
            {
                connector->repaintRequested = 0;
                connector->renderInterface.render(connector);
                connector->renderInterface.flipPage(connector);
                continue;
            }
        }

        if (connector->state == SRM_CONNECTOR_STATE_CHANGING_MODE)
        {
            if (connector->renderInterface.updateMode(connector))
                connector->state = SRM_CONNECTOR_STATE_INITIALIZED;
            else
                connector->state = SRM_CONNECTOR_STATE_REVERTING_MODE;
        }
        else if (connector->state == SRM_CONNECTOR_STATE_UNINITIALIZING)
        {
            connector->renderInterface.uninitialize(connector);
            connector->state = SRM_CONNECTOR_STATE_UNINITIALIZED;
            break;
        }
        else if (connector->state == SRM_CONNECTOR_STATE_PAUSING)
        {
            connector->state = SRM_CONNECTOR_STATE_PAUSED;
            connector->renderInterface.pause(connector);
            SRMDebug("[%s] Connector %d paused.", connector->device->rendererDevice->name, connector->id);
        }
        else if (connector->state == SRM_CONNECTOR_STATE_RESUMING)
        {
            connector->state = SRM_CONNECTOR_STATE_INITIALIZED;
            connector->renderInterface.resume(connector);
            SRMDebug("[%s] Connector %d resumed.", connector->device->rendererDevice->name, connector->id);
        }
    }

    return NULL;

    fail:
    pthread_mutex_destroy(&connector->repaintMutex);
    pthread_cond_destroy(&connector->repaintCond);
    connector->renderInitResult = -1;
    return NULL;
}

void srmConnectorUnlockRenderThread(SRMConnector *connector)
{
    pthread_mutex_lock(&connector->repaintMutex);
    connector->repaintRequested = 1;
    pthread_cond_signal(&connector->repaintCond);
    pthread_mutex_unlock(&connector->repaintMutex);
}
