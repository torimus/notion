/*
 * ion/ionws/ionws.c
 *
 * Copyright (c) Tuomo Valkonen 1999-2004. 
 *
 * Ion is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */

#include <string.h>

#include <libtu/objp.h>
#include <libtu/minmax.h>
#include <ioncore/common.h>
#include <ioncore/rootwin.h>
#include <ioncore/focus.h>
#include <ioncore/global.h>
#include <ioncore/region.h>
#include <ioncore/manage.h>
#include <ioncore/screen.h>
#include <ioncore/names.h>
#include <ioncore/saveload.h>
#include <ioncore/attach.h>
#include <ioncore/resize.h>
#include <ioncore/extl.h>
#include <ioncore/region-iter.h>
#include <ioncore/regbind.h>
#include <ioncore/extlconv.h>
#include <ioncore/defer.h>
#include "placement.h"
#include "ionws.h"
#include "ionframe.h"
#include "split.h"
#include "main.h"


/*{{{ region dynfun implementations */


static bool ionws_fitrep(WIonWS *ws, WWindow *par, const WFitParams *fp)
{
    WRegion *sub, *next;
    bool rs;
    
    if(par!=NULL){
        if(!region_same_rootwin((WRegion*)ws, (WRegion*)par))
            return FALSE;
    
        region_detach_parent((WRegion*)ws);
        region_attach_parent((WRegion*)ws, (WRegion*)par);
    
        FOR_ALL_MANAGED_ON_LIST_W_NEXT(ws->managed_list, sub, next){
            WFitParams subfp;
            subfp.g=REGION_GEOM(sub);
            subfp.mode=REGION_FIT_EXACT;
            if(!region_fitrep(sub, par, &subfp)){
                warn("Problem: can't reparent a %s managed by a WIonWS"
                     "being reparented. Detaching from this object.",
                     OBJ_TYPESTR(sub));
                region_detach_manager(sub);
            }
        }
    }
    
    REGION_GEOM(ws)=fp->g;
    
    if(ws->split_tree==NULL)
        return TRUE;
    
    split_resize(ws->split_tree, &(fp->g), PRIMN_ANY, PRIMN_ANY);
    
    return TRUE;
}


static void ionws_map(WIonWS *ws)
{
    WRegion *reg;

    REGION_MARK_MAPPED(ws);
    
    FOR_ALL_MANAGED_ON_LIST(ws->managed_list, reg){
        region_map(reg);
    }
}


static void ionws_unmap(WIonWS *ws)
{
    WRegion *reg;
    
    REGION_MARK_UNMAPPED(ws);
    
    FOR_ALL_MANAGED_ON_LIST(ws->managed_list, reg){
        region_unmap(reg);
    }
}


static void ionws_do_set_focus(WIonWS *ws, bool warp)
{
    WRegion *sub=ionws_current(ws);
    
    if(sub==NULL){
        warn("Trying to focus an empty ionws.");
        return;
    }

    region_do_set_focus(sub, warp);
}


static bool ionws_managed_display(WIonWS *ws, WRegion *reg)
{
    return TRUE;
}


/*}}}*/


/*{{{ Create/destroy */


static void ionws_managed_add(WIonWS *ws, WRegion *reg)
{
    region_set_manager(reg, (WRegion*)ws, &(ws->managed_list));
    
    region_add_bindmap_owned(reg, mod_ionws_ionws_bindmap, (WRegion*)ws);
    
    if(REGION_IS_MAPPED(ws))
        region_map(reg);
}


static WIonFrame *create_initial_frame(WIonWS *ws, WWindow *parent,
                                       const WFitParams *fp)
{
    WIonFrame *frame;
    
    frame=create_ionframe(parent, fp);

    if(frame==NULL)
        return NULL;
    
    ws->split_tree=create_split_regnode((WRegion*)frame, &(fp->g));
    if(ws->split_tree==NULL){
        warn_err();
        destroy_obj((Obj*)frame);
        return NULL;
    }
    
    ionws_managed_add(ws, (WRegion*)frame);

    return frame;
}


static bool ionws_init(WIonWS *ws, WWindow *parent, const WFitParams *fp,
                       bool ci)
{
    ws->managed_splits=extl_create_table();
    
    if(ws->managed_splits==extl_table_none())
        return FALSE;
    
    ws->split_tree=NULL;

    genws_init(&(ws->genws), parent, fp);
    
    if(ci){
        if(create_initial_frame(ws, parent, fp)==NULL){
            genws_deinit(&(ws->genws));
            extl_unref_table(ws->managed_splits);
            return FALSE;
        }
    }
    
    return TRUE;
}


WIonWS *create_ionws(WWindow *parent, const WFitParams *fp, bool ci)
{
    CREATEOBJ_IMPL(WIonWS, ionws, (p, parent, fp, ci));
}


WIonWS *create_ionws_simple(WWindow *parent, const WFitParams *fp)
{
    return create_ionws(parent, fp, TRUE);
}


void ionws_deinit(WIonWS *ws)
{
    WRegion *reg;
    
    while(ws->managed_list!=NULL)
        ionws_managed_remove(ws, ws->managed_list);

    genws_deinit(&(ws->genws));
    
    extl_unref_table(ws->managed_splits);
}


static bool ionws_managed_may_destroy(WIonWS *ws, WRegion *reg)
{
    if(ws->split_tree!=NULL && 
       ws->split_tree->type==SPLIT_REGNODE &&
       ws->split_tree->u.reg==reg){
        return region_may_destroy((WRegion*)ws);
    }else{
        return TRUE;
    }
}


bool ionws_rescue_clientwins(WIonWS *ws)
{
    return region_rescue_managed_clientwins((WRegion*)ws, ws->managed_list);
}


static WSplit *get_node_check(WIonWS *ws, WRegion *reg)
{
    WSplit *node;
    
    if(reg==NULL)
        return NULL;
    
    node=split_tree_node_of(reg);
    
    if(node==NULL || REGION_MANAGER(reg)!=(WRegion*)ws)
        return NULL;
    
    return node;
}


void ionws_managed_remove(WIonWS *ws, WRegion *reg)
{
    bool ds=OBJ_IS_BEING_DESTROYED(ws);
    WSplit *other, *node=get_node_check(ws, reg);
    
    /* This function should only be called by a code that gets us 
     * through reg 
     */
    assert(node!=NULL);
    
    region_unset_manager(reg, (WRegion*)ws, &(ws->managed_list));
    region_remove_bindmap_owned(reg, mod_ionws_ionws_bindmap, (WRegion*)ws);

    other=split_tree_remove(&(ws->split_tree), node, !ds);
    
    if(!ds){
        if(other){
            if(region_may_control_focus((WRegion*)ws)){
                /* Other is guaranteed to be SPLIT_REGNODE */
                region_set_focus(other->u.reg);
            }
        }else{
            ioncore_defer_destroy((Obj*)ws);
        }
    }
}


bool ionws_manage_rescue(WIonWS *ws, WClientWin *cwin, WRegion *from)
{
    WSplit *split;
    WMPlex *nmgr;
    
    if(REGION_MANAGER(from)!=(WRegion*)ws)
        return FALSE;

    nmgr=split_tree_find_mplex(from);

    if(nmgr!=NULL)
        return (NULL!=mplex_attach_simple(nmgr, (WRegion*)cwin, 0));
    
    return FALSE;
}


/*}}}*/


/*{{{ Resize */


void ionws_managed_rqgeom(WIonWS *ws, WRegion *mgd, 
                          int flags, const WRectangle *geom,
                          WRectangle *geomret)
{
    WSplit *node=get_node_check(ws, mgd);
    if(node!=NULL)
        split_tree_rqgeom(ws->split_tree, node, flags, geom, geomret);
}


/*EXTL_DOC
 * Attempt to resize and/or move the split tree starting at \var{node}
 * (\type{WSplit} or \type{WRegion}). Behaviour and the \var{g} 
 * parameter are as for \fnref{WRegion.rqgeom} operating on
 * \var{node} (if it were a \type{WRegion}).
 */
EXTL_EXPORT_MEMBER
ExtlTab ionws_resize_tree(WIonWS *ws, Obj *node, ExtlTab g)
{
    WRectangle geom, ogeom;
    int flags=REGION_RQGEOM_WEAK_ALL;
    WSplit *snode;
    
    if(node!=NULL && OBJ_IS(node, WRegion)){
        geom=REGION_GEOM((WRegion*)node);
        snode=get_node_check(ws, (WRegion*)node);
        if(snode==NULL)
            goto err;
    }else if(node!=NULL && OBJ_IS(node, WSplit)){
        snode=(WSplit*)node;
        geom=snode->geom;
    }else{
        goto err;
    }
    
    ogeom=geom;

    if(extl_table_gets_i(g, "x", &(geom.x)))
        flags&=~REGION_RQGEOM_WEAK_X;
    if(extl_table_gets_i(g, "y", &(geom.y)))
        flags&=~REGION_RQGEOM_WEAK_Y;
    if(extl_table_gets_i(g, "w", &(geom.w)))
        flags&=~REGION_RQGEOM_WEAK_W;
    if(extl_table_gets_i(g, "h", &(geom.h)))
        flags&=~REGION_RQGEOM_WEAK_H;
    
    geom.w=maxof(1, geom.w);
    geom.h=maxof(1, geom.h);

    split_tree_rqgeom(ws->split_tree, snode, flags, &geom, &ogeom);
    
    return extl_table_from_rectangle(&ogeom);
    
err:
    warn("Invalid node.");
    return extl_table_none();
}


/*}}}*/


/*{{{ Split/unsplit */


static bool get_split_dir_primn(const char *str, int *dir, int *primn)
{
    if(str==NULL)
        return FALSE;
    
    if(!strcmp(str, "left")){
        *primn=PRIMN_TL;
        *dir=SPLIT_HORIZONTAL;
    }else if(!strcmp(str, "right")){
        *primn=PRIMN_BR;
        *dir=SPLIT_HORIZONTAL;
    }else if(!strcmp(str, "top") || 
             !strcmp(str, "above") || 
             !strcmp(str, "up")){
        *primn=PRIMN_TL;
        *dir=SPLIT_VERTICAL;
    }else if(!strcmp(str, "bottom") || 
             !strcmp(str, "below") ||
             !strcmp(str, "down")){
        *primn=PRIMN_BR;
        *dir=SPLIT_VERTICAL;
    }else{
        return FALSE;
    }
    
    return TRUE;
}


/*EXTL_DOC
 * Create new WIonFrame on \var{ws} above/below/left of/right of
 * all other objects depending on \var{dirstr}
 * (one of ''left'', ''right'', ''top'' or ''bottom'').
 */
EXTL_EXPORT_MEMBER
WIonFrame *ionws_split_top(WIonWS *ws, const char *dirstr)
{
    int dir, primn, mins;
    WSplit *nnode=NULL;
    
    if(!get_split_dir_primn(dirstr, &dir, &primn))
        return NULL;
    
    mins=16; /* totally arbitrary */
    
    if(ws->split_tree==NULL)
        return NULL;
    
    nnode=split_tree_split(&(ws->split_tree), ws->split_tree, 
                           dir, primn, mins, PRIMN_ANY,
                           (WRegionSimpleCreateFn*)create_ionframe,
                           REGION_PARENT_CHK(ws, WWindow));
    
    if(nnode==NULL)
        return NULL;

    ionws_managed_add(ws, nnode->u.reg);
    region_warp(nnode->u.reg);
    
    return OBJ_CAST(nnode->u.reg, WIonFrame);
}


/*EXTL_DOC
 * Split \var{frame} creating a new WIonFrame to direction \var{dir}
 * (one of ''left'', ''right'', ''top'' or ''bottom'') of \var{frame}.
 * If \var{attach_current} is set, the region currently displayed in
 * \var{frame}, if any, is moved to thenew frame.
 */
EXTL_EXPORT_MEMBER
WIonFrame *ionws_split_at(WIonWS *ws, WIonFrame *frame, const char *dirstr, 
                          bool attach_current)
{
    WRegion *curr;
    int dir, primn, mins;
    WSplit *node, *nnode;
    
    node=get_node_check(ws, (WRegion*)frame);
    
    if(node==NULL){
        warn_obj("ionws_split_at", "Invalid frame or frame not managed by "
                 "the workspace.");
        return NULL;
    }
    
    if(!get_split_dir_primn(dirstr, &dir, &primn)){
        warn_obj("ionws_split_at", "Unknown direction parameter to split_at");
        return NULL;
    }
    
    mins=(dir==SPLIT_VERTICAL
          ? region_min_h((WRegion*)frame)
          : region_min_w((WRegion*)frame));
    
    nnode=split_tree_split(&(ws->split_tree), node, dir, primn, mins, primn,
                           (WRegionSimpleCreateFn*)create_ionframe,
                           REGION_PARENT_CHK(ws, WWindow));
    
    if(nnode==NULL){
        warn_obj("ionws_split_at", "Unable to split");
        return NULL;
    }

    assert(OBJ_IS(nnode->u.reg, WIonFrame));

    ionws_managed_add(ws, nnode->u.reg);
    
    curr=mplex_l1_current(&(frame->frame.mplex));
    
    if(attach_current && curr!=NULL){
        mplex_attach_simple((WMPlex*)nnode->u.reg, curr, 
                            MPLEX_ATTACH_SWITCHTO);
    }
    
    if(region_may_control_focus((WRegion*)frame))
        region_goto(nnode->u.reg);

    return (WIonFrame*)nnode->u.reg;
}


/*EXTL_DOC
 * Try to relocate regions managed by \var{frame} to another frame
 * and, if possible, destroy the frame.
 */
EXTL_EXPORT_MEMBER
void ionws_unsplit_at(WIonWS *ws, WIonFrame *frame)
{
    if(frame==NULL){
        warn_obj("ionws_unsplit_at", "nil frame");
        return;
    }
    if(REGION_MANAGER(frame)!=(WRegion*)ws){
        warn_obj("ionws_unsplit_at", "The frame is not managed by the workspace.");
        return;
    }
    
    if(!region_may_destroy((WRegion*)frame)){
        warn_obj("ionws_unsplit_at", "Frame may not be destroyed");
        return;
    }

    if(!region_rescue_clientwins((WRegion*)frame)){
        warn_obj("ionws_unsplit_at", "Failed to rescue managed objects.");
        return;
    }

    ioncore_defer_destroy((Obj*)frame);
}


/*}}}*/


/*{{{ Navigation etc. exports */


/*EXTL_DOC
 * Returns most recently active region on \var{ws}.
 */
EXTL_EXPORT_MEMBER
WRegion *ionws_current(WIonWS *ws)
{
    WSplit *node=split_current_tl(ws->split_tree, -1);
    return (node ? node->u.reg : NULL);
}


/*EXTL_DOC
 * Returns a list of regions managed by the workspace (frames, mostly).
 */
EXTL_EXPORT_MEMBER
ExtlTab ionws_managed_list(WIonWS *ws)
{
    return managed_list_to_table(ws->managed_list, NULL);
}


/*EXTL_DOC
 * Returns the root of the split tree.
 */
EXTL_EXPORT_MEMBER
Obj *ionws_split_tree(WIonWS *ws)
{
    return (ws->split_tree==NULL ? NULL : split_hoist(ws->split_tree));
}


static WRegion *do_get_next_to(WIonWS *ws, WRegion *reg, int dir, int primn)
{
    WSplit *node=get_node_check(ws, reg);
    
    if(node!=NULL){
        if(primn==PRIMN_TL)
            node=split_to_tl(node, dir);
        else
            node=split_to_br(node, dir);
    }
    return (node ? node->u.reg : NULL);
}


/*EXTL_DOC
 * Return the most previously active region next to \var{reg} in
 * direction \var{dirstr} (left/right/up/down). The region \var{reg}
 * must be managed by \var{ws}.
 */
EXTL_EXPORT_MEMBER
WRegion *ionws_next_to(WIonWS *ws, WRegion *reg, const char *dirstr)
{
    int dir=0, primn=0;
    
    if(!get_split_dir_primn(dirstr, &dir, &primn))
        return NULL;
    
    return do_get_next_to(ws, reg, dir, primn);
}


static WRegion *do_get_farthest(WIonWS *ws, int dir, int primn)
{
    WSplit *node;
    
    if(primn==PRIMN_TL)
        node=split_current_tl(ws->split_tree, dir);
    else
        node=split_current_br(ws->split_tree, dir);
    
    return (node ? node->u.reg : NULL);
}


/*EXTL_DOC
 * Return the most previously active region on \var{ws} with no
 * other regions next to it in  direction \var{dirstr} 
 * (left/right/up/down). 
 */
EXTL_EXPORT_MEMBER
WRegion *ionws_farthest(WIonWS *ws, const char *dirstr)
{
    int dir=0, primn=0;

    if(!get_split_dir_primn(dirstr, &dir, &primn))
        return NULL;
    
    return do_get_farthest(ws, dir, primn);
}


static WRegion *do_goto_dir(WIonWS *ws, int dir, int primn)
{
    int primn2=(primn==PRIMN_TL ? PRIMN_BR : PRIMN_TL);
    WRegion *reg=NULL, *curr=ionws_current(ws);
    if(curr!=NULL)
        reg=do_get_next_to(ws, curr, dir, primn);
    if(reg==NULL)
        reg=do_get_farthest(ws, dir, primn2);
    if(reg!=NULL)
        region_goto(reg);
    return reg;
}


/*EXTL_DOC
 * Go to the most previously active region on \var{ws} next to \var{reg} in
 * direction \var{dirstr} (up/down/left/right), wrapping around to a most 
 * recently active farthest region in the opposite direction if \var{reg} 
 * is already the further region in the given direction.
 * 
 * Note that this function is asynchronous; the region will not
 * actually have received the focus when this function returns.
 */
EXTL_EXPORT_MEMBER
WRegion *ionws_goto_dir(WIonWS *ws, const char *dirstr)
{
    int dir=0, primn=0;

    if(!get_split_dir_primn(dirstr, &dir, &primn))
        return NULL;
    
    return do_goto_dir(ws, dir, primn);
}


static WRegion *do_goto_dir_nowrap(WIonWS *ws, int dir, int primn)
{
    int primn2=(primn==PRIMN_TL ? PRIMN_BR : PRIMN_TL);
    WRegion *reg=NULL, *curr=ionws_current(ws);
    if(curr!=NULL)
        reg=do_get_next_to(ws, curr, dir, primn);
    if(reg!=NULL)
        region_goto(reg);
    return reg;
}


/*EXTL_DOC
 * Go to the most previously active region on \var{ws} next to \var{reg} in
 * direction \var{dirstr} (up/down/left/right) without wrapping around.
 */
EXTL_EXPORT_MEMBER
WRegion *ionws_goto_dir_nowrap(WIonWS *ws, const char *dirstr)
{
    int dir=0, primn=0;

    if(!get_split_dir_primn(dirstr, &dir, &primn))
        return NULL;
    
    return do_goto_dir_nowrap(ws, dir, primn);
}


/*EXTL_DOC
 * Find region on \var{ws} overlapping coordinates $(x, y)$.
 */
EXTL_EXPORT_MEMBER
WRegion *ionws_region_at(WIonWS *ws, int x, int y)
{
    return split_region_at(ws->split_tree, x, y);
}


/*EXTL_DOC
 * For region \var{reg} managed by \var{ws} return the \type{WSplit}
 * a leaf of which \var{reg} is.
 */
EXTL_EXPORT_MEMBER
WSplit *ionws_split_of(WIonWS *ws, WRegion *reg)
{
    if(reg==NULL){
        warn_obj("ionws_split_of", "nil parameter");
        return NULL;
    }
    
    if(REGION_MANAGER(reg)!=(WRegion*)ws){
        warn_obj("ionws_split_of", "Manager doesn't match");
        return NULL;
    }
    
    return split_tree_split_of(reg);
}


/*}}}*/


/*{{{ Misc. */


void ionws_managed_activated(WIonWS *ws, WRegion *reg)
{
    WSplit *node=get_node_check(ws, reg);
    if(node!=NULL)
        split_mark_current(node);
}


/*}}}*/


/*{{{ Save */


static ExtlTab get_node_config(WSplit *node)
{
    int tls, brs;
    ExtlTab tab, stab;
    
    assert(node!=NULL);
    
    if(node->type==SPLIT_REGNODE){
        if(region_supports_save(node->u.reg))
            return region_get_configuration(node->u.reg);
        return extl_table_none();
    }
    
    tab=extl_create_table();
    
    tls=split_size(node->u.s.tl, node->type);
    brs=split_size(node->u.s.br, node->type);
    
    extl_table_sets_s(tab, "split_dir",(node->type==SPLIT_VERTICAL
                                        ? "vertical" : "horizontal"));
    
    extl_table_sets_i(tab, "split_tls", tls);
    extl_table_sets_i(tab, "split_brs", brs);
    
    stab=get_node_config(node->u.s.tl);
    if(stab==extl_table_none()){
        warn("Could not get configuration for split TL (a %s).", 
             OBJ_TYPESTR(node->u.s.tl));
    }else{
        extl_table_sets_t(tab, "tl", stab);
        extl_unref_table(stab);
    }
    
    stab=get_node_config(node->u.s.br);
    if(stab==extl_table_none()){
        warn("Could not get configuration for split BR (a %s).", 
             OBJ_TYPESTR(node->u.s.br));
    }else{
        extl_table_sets_t(tab, "br", stab);
        extl_unref_table(stab);
    }

    return tab;
}


static ExtlTab ionws_get_configuration(WIonWS *ws)
{
    ExtlTab tab, split_tree;
    
    tab=region_get_base_configuration((WRegion*)ws);
    split_tree=get_node_config(ws->split_tree);
    
    if(split_tree==extl_table_none()){
        warn("Could not get split tree for a WIonWS.");
    }else{
        extl_table_sets_t(tab, "split_tree", split_tree);
        extl_unref_table(split_tree);
    }
    
    return tab;
}


/*}}}*/


/*{{{ Load */


extern void set_split_of(Obj *obj, WSplit *split);
static WSplit *load_node(WIonWS *ws, WWindow *par, const WRectangle *geom, 
                         ExtlTab tab);

#define MINS 8

static WSplit *load_split(WIonWS *ws, WWindow *par, const WRectangle *geom,
                          ExtlTab tab)
{
    WSplit *split;
    char *dir_str;
    int dir, brs, tls;
    ExtlTab subtab;
    WSplit *tl=NULL, *br=NULL;
    WRectangle geom2;

    if(!extl_table_gets_i(tab, "split_tls", &tls))
        return FALSE;
    if(!extl_table_gets_i(tab, "split_brs", &brs))
        return FALSE;
    if(!extl_table_gets_s(tab, "split_dir", &dir_str))
        return FALSE;
    if(strcmp(dir_str, "vertical")==0){
        dir=SPLIT_VERTICAL;
    }else if(strcmp(dir_str, "horizontal")==0){
        dir=SPLIT_HORIZONTAL;
    }else{
        free(dir_str);
        return NULL;
    }
    free(dir_str);

    split=create_split(dir, NULL, NULL, geom);
    if(split==NULL){
        warn("Unable to create a split.\n");
        return NULL;
    }

    tls=maxof(tls, MINS);
    brs=maxof(brs, MINS);
        
    geom2=*geom;
    if(dir==SPLIT_HORIZONTAL){
        tls=maxof(0, geom->w)*tls/(tls+brs);
        geom2.w=tls;
    }else{
        tls=maxof(0, geom->h)*tls/(tls+brs);
        geom2.h=tls;
    }
    
    if(extl_table_gets_t(tab, "tl", &subtab)){
        tl=load_node(ws, par, &geom2, subtab);
        extl_unref_table(subtab);
    }

    geom2=*geom;
    if(tl!=NULL){
        if(dir==SPLIT_HORIZONTAL){
            geom2.w-=tls;
            geom2.x+=tls;
        }else{
            geom2.h-=tls;
            geom2.y+=tls;
        }
    }
            
    if(extl_table_gets_t(tab, "br", &subtab)){
        br=load_node(ws, par, &geom2, subtab);
        extl_unref_table(subtab);
    }
    
    if(tl==NULL || br==NULL){
        free(split);
        return (tl==NULL ? br : tl);
    }
    
    tl->parent=split;
    br->parent=split;

    /*split->tmpsize=tls;*/
    split->u.s.tl=tl;
    split->u.s.br=br;
    
    return split;
}


static WSplit *load_node(WIonWS *ws, WWindow *par, const WRectangle *geom,
                         ExtlTab tab)
{
    char *typestr;
    
    if(extl_table_gets_s(tab, "type", &typestr)){
        WRegion *reg;
        WFitParams fp;
        WSplit *node;
        
        free(typestr);
        
        fp.g=*geom;
        fp.mode=REGION_FIT_EXACT;
        reg=create_region_load(par, &fp, tab);
        if(reg==NULL)
            return NULL;
        
        node=create_split_regnode(reg, geom);
        if(node==NULL){
            destroy_obj((Obj*)reg);
            return NULL;
        }
        
        ionws_managed_add(ws, reg);
        return node;
    }
    
    return load_split(ws, par, geom, tab);
}


WRegion *ionws_load(WWindow *par, const WFitParams *fp, ExtlTab tab)
{
    WIonWS *ws;
    ExtlTab treetab;
    bool ci=TRUE;

    if(extl_table_gets_t(tab, "split_tree", &treetab))
        ci=FALSE;
    
    ws=create_ionws(par, fp, ci);
    
    if(ws==NULL){
        if(!ci)
            extl_unref_table(treetab);
        return NULL;
    }

    if(!ci){
        ws->split_tree=load_node(ws, par, &REGION_GEOM(ws), treetab);
        extl_unref_table(treetab);
    }
    
    if(ws->split_tree==NULL){
        warn("Workspace empty");
        destroy_obj((Obj*)ws);
        return NULL;
    }
    
    return (WRegion*)ws;
}


/*}}}*/


/*{{{�Dynamic function table and class implementation */


static DynFunTab ionws_dynfuntab[]={
    {region_map, ionws_map},
    {region_unmap, ionws_unmap},
    {region_do_set_focus, ionws_do_set_focus},
    
    {(DynFun*)region_fitrep,
     (DynFun*)ionws_fitrep},
    
    {region_managed_rqgeom, ionws_managed_rqgeom},
    {region_managed_activated, ionws_managed_activated},
    {region_managed_remove, ionws_managed_remove},
    
    {(DynFun*)region_managed_display,
     (DynFun*)ionws_managed_display},
    
    {(DynFun*)region_manage_clientwin, 
     (DynFun*)ionws_manage_clientwin},
    {(DynFun*)region_manage_rescue,
     (DynFun*)ionws_manage_rescue},
    
    {(DynFun*)region_rescue_clientwins,
     (DynFun*)ionws_rescue_clientwins},
    
    {(DynFun*)region_get_configuration,
     (DynFun*)ionws_get_configuration},

    {(DynFun*)region_managed_may_destroy,
     (DynFun*)ionws_managed_may_destroy},

    {(DynFun*)region_current,
     (DynFun*)ionws_current},

    END_DYNFUNTAB
};


IMPLCLASS(WIonWS, WGenWS, ionws_deinit, ionws_dynfuntab);

    
/*}}}*/
