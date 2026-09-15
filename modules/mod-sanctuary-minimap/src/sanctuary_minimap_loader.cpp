/*
 * mod-sanctuary-minimap - script loader
 *
 * Tracking several things on the minimap at once. The spell kinds need nothing from the
 * server and are the addon's own work; the PLACE kinds - mailbox, banker, repair and the
 * rest - need to be told where anything is, because 3.3.5 gives Lua no world position at
 * all. That is what SanctuaryMinimap.cpp sends.
 *
 * The addon lives here too (addon/SanctuaryMinimap) because make-dist.ps1 collects addons
 * only from the module tree.
 *
 * The name of this function is derived from the module directory name by modules/CMakeLists.txt;
 * do not rename it.
 */

void AddSC_sanctuary_minimap();

void Addmod_sanctuary_minimapScripts()
{
    AddSC_sanctuary_minimap();
}
