/*
 * mod-sanctuary-outlaw - script loader
 *
 * The name of this function is derived from the module directory name by
 * modules/CMakeLists.txt; do not rename it.
 */

void AddSC_sanctuary_outlaw_scripts();
void AddSC_sanctuary_outlaw_commandscript();

void Addmod_sanctuary_outlawScripts()
{
    AddSC_sanctuary_outlaw_scripts();
    AddSC_sanctuary_outlaw_commandscript();
}
