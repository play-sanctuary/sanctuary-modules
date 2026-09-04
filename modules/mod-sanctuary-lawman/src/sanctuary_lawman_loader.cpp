/*
 * mod-sanctuary-lawman - script loader
 *
 * The name of this function is derived from the module directory name by
 * modules/CMakeLists.txt; do not rename it.
 */

void AddSC_sanctuary_lawman_scripts();
void AddSC_sanctuary_lawman_commandscript();
void AddSC_sanctuary_lawman_shackles();

void Addmod_sanctuary_lawmanScripts()
{
    AddSC_sanctuary_lawman_scripts();
    AddSC_sanctuary_lawman_commandscript();
    AddSC_sanctuary_lawman_shackles();
}
