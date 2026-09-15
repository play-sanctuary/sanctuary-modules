/*
 * mod-sanctuary-faction - script loader
 *
 * The name of this function is derived from the module directory name by
 * modules/CMakeLists.txt; do not rename it.
 */

void AddSC_sanctuary_faction_scripts();
void AddSC_sanctuary_faction_commandscript();
void AddSC_sanctuary_faction_addonscript();

void Addmod_sanctuary_factionScripts()
{
    AddSC_sanctuary_faction_scripts();
    AddSC_sanctuary_faction_commandscript();
    AddSC_sanctuary_faction_addonscript();
}
