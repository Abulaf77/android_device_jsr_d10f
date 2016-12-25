#include <stdlib.h>
#include <stdio.h>

// #include "vendor_init.h"
#include "property_service.h"
#include "log.h"
#include "init.h"
#include "util.h"
#include "init.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/mman.h>
#include "service.h"

#define FALSE 0
#define TRUE 1

#define PERSISTENT_PROPERTY_DIR  "/data/property"
#define PERSISTENT_PROPERTY_CONFIGURATION_NAME "persist.storages.configuration"
#define STORAGES_CONFIGURATION_CLASSIC   "0"
#define STORAGES_CONFIGURATION_INVERTED  "1"
#define STORAGES_CONFIGURATION_DATAMEDIA "2"
#define SERVICE_VOLD "vold"

#define STORAGE_XML_PATH "/data/system/storage.xml"
#define STORAGE_XML_TMP_PATH "/data/system/storage.tmp"
#define STORAGE_XML_HEADER "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n"
#define STORAGE_XML_VOLUMES_TOKEN "<volumes version=\""
#define STORAGE_XML_PRIMARY_PHYSICAL_UUID_TOKEN "primaryStorageUuid=\"primary_physical\" "

#define STORAGE_XML_CONFIG_CLASSIC_FMT   "<volumes version=\"%d\" primaryStorageUuid=\"primary_physical\" forceAdoptable=\"%s\">\n"
#define STORAGE_XML_CONFIG_DATAMEDIA_FMT "<volumes version=\"%d\" forceAdoptable=\"%s\">\n"

#define USBMSC_PRESENT_PROPERTY_NAME "ro.usbmsc.present"
#define USBMSC_PARTITION_PATH "/dev/block/platform/msm_sdcc.1/by-name/usbmsc"

char *open_xml_configuration(off_t *size) {
	FILE *xml_config_filestream = fopen(STORAGE_XML_PATH, "r+");
	if (xml_config_filestream == NULL) {
		ERROR("Unable to open storage configuration XML \"%s\" - storages may be inconsistent (errno: %d (%s))!\n",
		      STORAGE_XML_PATH, errno, strerror(errno));
		return NULL;
	}

	fseeko(xml_config_filestream, 0l, SEEK_END);
	*size = ftello(xml_config_filestream)+strlen(STORAGE_XML_PRIMARY_PHYSICAL_UUID_TOKEN); // some space for expansion
	fseeko(xml_config_filestream, 0l, SEEK_SET);

	char *config = (char *) mmap(NULL, *size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fileno(xml_config_filestream), 0);
	if (config == MAP_FAILED) {
		ERROR("Unable to mmap storage configuration XML \"%s\" - storages may be inconsistent (errno: %d (%s))!\n", STORAGE_XML_PATH, errno, strerror(errno));
		return NULL;
	}
	fclose(xml_config_filestream); // mmaped data still available
	return config;
}

void update_xml_configuration(char *xml_config, off_t config_size, int isDatamedia) {
	char *volumes=NULL, *version=NULL;
	version = strstr(xml_config, STORAGE_XML_HEADER);
	volumes = strstr(xml_config, STORAGE_XML_VOLUMES_TOKEN);
	if (version == NULL || volumes == NULL) {
		ERROR("Storage config xml \"%s\" looks werid - erasing it\n", STORAGE_XML_PATH);
		unlink(STORAGE_XML_PATH);
		return;
	}

	int scanned_size=0, fields=0, iversion=0, buffer_size=0, printed_bytes, buffer_free_bytes=0;
	char primaryStorageUuid[64+1], forceAdoptable[64+1];
	char *data=NULL, *buffer=NULL, *rest_of_config=NULL;

	buffer_size=config_size + strlen(STORAGE_XML_PRIMARY_PHYSICAL_UUID_TOKEN); // needed if migrating from datamedia to classic
	buffer = data = (char *) calloc(1, buffer_size);
	if (buffer == NULL) {
		ERROR("Out of memory while allocating %d bytes\n", buffer_size);
		return;
	}

	data += sprintf(data, STORAGE_XML_HEADER); // initialize config header

	if (isDatamedia) {
		// only datamedia needs emulated storage!
		fields = sscanf(volumes,
		                "<volumes version=\"%d\" forceAdoptable=\"%64[^\"]\">\n%n",
		                &iversion, forceAdoptable, &scanned_size);
		if (fields == 2) {
			free (buffer);
			return; // nothing to do here!
		}

		ERROR("Storage config xml \"%s\" needs updating (looks like classic)!\n", STORAGE_XML_PATH);
		fields = sscanf(volumes,
		                "<volumes version=\"%d\" primaryStorageUuid=\"%64[^\"]\" forceAdoptable=\"%64[^\"]\">\n%n",
		                &iversion, primaryStorageUuid, forceAdoptable, &scanned_size);
		if (fields != 3) {
			ERROR("Storage config xml \"%s\" is werid - got %d fields (want 3)!\n", STORAGE_XML_PATH, fields);
			free (buffer);
			return;
		}
		printed_bytes=sprintf(data, STORAGE_XML_CONFIG_DATAMEDIA_FMT, iversion, forceAdoptable);
		data += printed_bytes;
		buffer_free_bytes=buffer_size-strlen(STORAGE_XML_HEADER)-printed_bytes;
	} else {
		fields = sscanf(volumes,
		                "<volumes version=\"%d\" primaryStorageUuid=\"%64[^\"]\" forceAdoptable=\"%64[^\"]\">\n%n",
		                &iversion, primaryStorageUuid, forceAdoptable, &scanned_size);
		if (fields == 3) {
			free (buffer);
			return; // nothing to do here!
		}

		ERROR("Storage config xml \"%s\" needs updating (looks like datamedia)!\n", STORAGE_XML_PATH);
		fields = sscanf(volumes,
		                "<volumes version=\"%d\" forceAdoptable=\"%64[^\"]\">\n%n",
		                &iversion, forceAdoptable, &scanned_size);
		if (fields != 2) {
			ERROR("Storage config xml \"%s\" is werid - got %d fields (want 2)!\n", STORAGE_XML_PATH, fields);
			free (buffer);
			return; // nothing to do here!
		}
		buffer_free_bytes=buffer_size - strlen(STORAGE_XML_HEADER) - scanned_size
		                  - strlen (STORAGE_XML_PRIMARY_PHYSICAL_UUID_TOKEN) - 1;
		printed_bytes = sprintf(data, STORAGE_XML_CONFIG_CLASSIC_FMT, iversion, forceAdoptable);
		data += printed_bytes;
	}
	// just copy rest of config data
	rest_of_config=xml_config + strlen(STORAGE_XML_HEADER) + scanned_size;
	strncpy(data, rest_of_config, buffer_free_bytes);
	ERROR("Going to write '%s' into config %s\n", buffer, STORAGE_XML_PATH);
	ERROR("Updating %s\n", STORAGE_XML_PATH);
	munmap(xml_config, config_size);
	FILE *storage_config = fopen(STORAGE_XML_PATH, "w");
	if (storage_config == NULL) {
		ERROR("Unable to open %s for writing (errno=%d: %s)\n", STORAGE_XML_PATH, errno, strerror(errno));
		free(buffer);
		return;
	}
	fwrite(buffer, strlen(buffer)-1, 1, storage_config);
	fclose(storage_config);
	free (buffer);
}

// TODO: eliminate this!
void vendor_load_properties(void)
{
	std::string value;

	DIR * dir = opendir(PERSISTENT_PROPERTY_DIR);
	int dir_fd;
	struct dirent * entry;
	int fd, length, isDatamedia=FALSE;
	struct stat sb;

	if (dir) {
		dir_fd = dirfd(dir);
		while ((entry = readdir(dir)) != NULL) {
			// we need to read this properties before load_persistent_properties()
			if (strncmp(PERSISTENT_PROPERTY_CONFIGURATION_NAME, entry->d_name, strlen(PERSISTENT_PROPERTY_CONFIGURATION_NAME)))
				continue;
#if HAVE_DIRENT_D_TYPE
			if (entry->d_type != DT_REG)
				continue;
#endif
			/* open the file and read the property value */
			fd = openat(dir_fd, entry->d_name, O_RDONLY | O_NOFOLLOW);
			if (fd < 0) {
				ERROR("Unable to open persistent property file \"%s\" errno: %d\n", entry->d_name, errno);
				continue;
			}
			if (fstat(fd, &sb) < 0) {
				ERROR("fstat on property file \"%s\" failed errno: %d\n", entry->d_name, errno);
				close(fd);
				continue;
			}

			// File must not be accessible to others, be owned by root/root, and
			// not be a hard link to any other file.
			if (((sb.st_mode & (S_IRWXG | S_IRWXO)) != 0)
				      || (sb.st_uid != 0)
				      || (sb.st_gid != 0)
				      || (sb.st_nlink != 1)) {
				ERROR("skipping insecure property file %s (uid=%u gid=%u nlink=%d mode=%o)\n",
				      entry->d_name, (unsigned int)sb.st_uid, (unsigned int)sb.st_gid,
				      sb.st_nlink, sb.st_mode);
				close(fd);
				continue;
			}
                        char temp[PROP_VALUE_MAX];
			length = read(fd, temp, sizeof(temp) - 1);
			if (length >= 0) {
				temp[length] = 0;
				property_set(entry->d_name, temp);
			} else {
				ERROR("Unable to read persistent property file %s errno: %d\n", entry->d_name, errno);
			}
			value = temp;
			close(fd);
		}
		closedir(dir);
	} else {
		ERROR("Unable to open persistent property directory %s errno: %d\n", PERSISTENT_PROPERTY_DIR, errno);
	}

	errno=0;

        int usbmsc_present = FALSE;
        if (access("/dev/block/platform/msm_sdcc.2/by-name/usbmsc", F_OK) == 0)
            usbmsc_present = TRUE;
        else if (access("/dev/block/platform/msm_sdcc.1/by-name/usbmsc", F_OK) == 0)
            usbmsc_present = TRUE;
        
	if (usbmsc_present) {
	    ERROR("usbmsc present\n");
	    property_set(USBMSC_PRESENT_PROPERTY_NAME, "true");
        } else {
		ERROR("usbmsc NOT present\n");
		property_set(USBMSC_PRESENT_PROPERTY_NAME, "false");
		isDatamedia = TRUE;
        }

	value = property_get(PERSISTENT_PROPERTY_CONFIGURATION_NAME);
	if (value == STORAGES_CONFIGURATION_DATAMEDIA) {
		// if datamedia
		ERROR("Got datamedia storage configuration (" PERSISTENT_PROPERTY_CONFIGURATION_NAME " == %s)\n", value.c_str());
		isDatamedia = TRUE;
	} else if (value == STORAGES_CONFIGURATION_INVERTED) {
		// if swapped
		property_set("ro.vold.primary_physical", "1");
		ERROR("Got inverted storage configuration (" PERSISTENT_PROPERTY_CONFIGURATION_NAME " == %s)\n", value.c_str());
	} else {
		// if classic (default case)
		ERROR("Got classic storage configuration (" PERSISTENT_PROPERTY_CONFIGURATION_NAME " == %s)\n", value.c_str());
		property_set("ro.vold.primary_physical", "1");
		if (isDatamedia) {
			value = STORAGES_CONFIGURATION_DATAMEDIA;
			property_set(PERSISTENT_PROPERTY_CONFIGURATION_NAME, value.c_str());
			ERROR("No usbmsc partiton - overriding storage configuration to datamedia! (" PERSISTENT_PROPERTY_CONFIGURATION_NAME " == %s)\n", value.c_str());
		}
	}

        off_t size;
        char *xml_config=open_xml_configuration(&size);
	if (xml_config != NULL) {
		update_xml_configuration(xml_config, size, isDatamedia);
		munmap(xml_config, size);
	}

	ERROR("Storages configuration applied\n");
}
