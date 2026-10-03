#include "dmod.h"
#include "libtodmv.h"

/**
 * @brief Main function of the application
 * 
 * @param argc Number of arguments
 * @param argv Array of arguments
 * 
 * @return 0 if success, error code otherwise
 */
int main(int argc, char *argv[])
{
    if(argc < 3)
    {
        DMOD_LOG_ERROR("Usage: todmv in_fname, out_fname\n");
        return -1;
    }

    

    return 0;
}
