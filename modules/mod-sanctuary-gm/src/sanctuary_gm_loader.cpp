/*
 * mod-sanctuary-gm - script loader
 *
 * The name of this function is derived from the module directory name by modules/CMakeLists.txt;
 * do not rename it.
 *
 * AddSC_sanctuary_gm_class() is gone: the game-master class (Timekeeper, class 10) was removed
 * on 2026-09-16. The panel and the command policy have nothing to do with it and stay.
 */

void AddSC_sanctuary_gm_scripts();
void AddSC_sanctuary_command_policy();

void Addmod_sanctuary_gmScripts()
{
    AddSC_sanctuary_gm_scripts();
    AddSC_sanctuary_command_policy();
}
