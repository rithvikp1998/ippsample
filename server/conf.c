/*
 * Configuration file support for sample IPP server implementation.
 *
 * Copyright © 2015-2018 by the IEEE-ISTO Printer Working Group
 * Copyright © 2015-2018 by Apple Inc.
 *
 * Licensed under Apache License v2.0.  See the file "LICENSE" for more
 * information.
 */

#include "ippserver.h"
#include <cups/file.h>
#include <cups/dir.h>
#include <fnmatch.h>
#include <grp.h>
#include <cups/ipp-private.h>


/*
 * Local globals...
 */

static _cups_mutex_t	printer_mutex = _CUPS_MUTEX_INITIALIZER;


/*
 * Local functions...
 */

static void		add_document_privacy(void);
static void		add_job_privacy(void);
static void		add_subscription_privacy(void);
static int		attr_cb(_ipp_file_t *f, server_pinfo_t *pinfo, const char *attr);
static int		compare_lang(server_lang_t *a, server_lang_t *b);
static int		compare_printers(server_printer_t *a, server_printer_t *b);
static server_lang_t	*copy_lang(server_lang_t *a);
#ifdef HAVE_AVAHI
static void		dnssd_client_cb(AvahiClient *c, AvahiClientState state, void *userdata);
#endif /* HAVE_AVAHI */
static int		error_cb(_ipp_file_t *f, server_pinfo_t *pinfo, const char *error);
static void		free_lang(server_lang_t *a);
static int		load_system(const char *conf);
static int		token_cb(_ipp_file_t *f, _ipp_vars_t *vars, server_pinfo_t *pinfo, const char *token);


/*
 * 'serverCleanAllJobs()' - Clean old jobs for all printers...
 */

void
serverCleanAllJobs(void)
{
  server_printer_t  *printer;             /* Current printer */


  serverLog(SERVER_LOGLEVEL_DEBUG, "Cleaning old jobs.");

  _cupsMutexLock(&printer_mutex);

  for (printer = (server_printer_t *)cupsArrayFirst(Printers); printer; printer = (server_printer_t *)cupsArrayNext(Printers))
    serverCleanJobs(printer);

  _cupsMutexUnlock(&printer_mutex);
}


/*
 * 'serverDNSSDInit()' - Initialize DNS-SD registrations.
 */

void
serverDNSSDInit(void)
{
#ifdef HAVE_DNSSD
  if (DNSServiceCreateConnection(&DNSSDMaster) != kDNSServiceErr_NoError)
  {
    fputs("Error: Unable to initialize Bonjour.\n", stderr);
    exit(1);
  }

#elif defined(HAVE_AVAHI)
  int error;			/* Error code, if any */

  if ((DNSSDMaster = avahi_threaded_poll_new()) == NULL)
  {
    fputs("Error: Unable to initialize Bonjour.\n", stderr);
    exit(1);
  }

  if ((DNSSDClient = avahi_client_new(avahi_threaded_poll_get(DNSSDMaster), AVAHI_CLIENT_NO_FAIL, dnssd_client_cb, NULL, &error)) == NULL)
  {
    fputs("Error: Unable to initialize Bonjour.\n", stderr);
    exit(1);
  }

  avahi_threaded_poll_start(DNSSDMaster);
#endif /* HAVE_DNSSD */
}


/*
 * 'serverFinalizeConfiguration()' - Make final configuration choices.
 */

int					/* O - 1 on success, 0 on failure */
serverFinalizeConfiguration(void)
{
  char	local[1024];			/* Local hostname */


 /*
  * Default hostname...
  */

  if (!ServerName && httpGetHostname(NULL, local, sizeof(local)))
    ServerName = strdup(local);

  if (!ServerName)
    ServerName = strdup("localhost");

#ifdef HAVE_SSL
 /*
  * Setup TLS certificate for server...
  */

  cupsSetServerCredentials(KeychainPath, ServerName, 1);
#endif /* HAVE_SSL */

 /*
  * Default directories...
  */

  if (!DataDirectory)
  {
    char	directory[1024];	/* New directory */
    const char	*tmpdir;		/* Temporary directory */

#ifdef WIN32
    if ((tmpdir = getenv("TEMP")) == NULL)
      tmpdir = "C:/TEMP";
#elif defined(__APPLE__)
    if ((tmpdir = getenv("TMPDIR")) == NULL)
      tmpdir = "/private/tmp";
#else
    if ((tmpdir = getenv("TMPDIR")) == NULL)
      tmpdir = "/tmp";
#endif /* WIN32 */

    snprintf(directory, sizeof(directory), "%s/ippserver.%d", tmpdir, (int)getpid());

    if (mkdir(directory, 0755) && errno != EEXIST)
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Unable to create default data directory \"%s\": %s", directory, strerror(errno));
      return (0);
    }

    serverLog(SERVER_LOGLEVEL_INFO, "Using default data directory \"%s\".", directory);

    DataDirectory = strdup(directory);
  }

  if (!SpoolDirectory)
  {
    SpoolDirectory = strdup(DataDirectory);

    serverLog(SERVER_LOGLEVEL_INFO, "Using default spool directory \"%s\".", DataDirectory);
  }

 /*
  * Authentication/authorization support...
  */

  if (Authentication)
  {
    if (AuthAdminGroup == SERVER_GROUP_NONE)
      AuthAdminGroup = SERVER_GROUP_WHEEL;
    if (AuthOperatorGroup == SERVER_GROUP_NONE)
      AuthOperatorGroup = getgid();

    if (!AuthName)
      AuthName = strdup("Printing");
    if (!AuthService && !AuthTestPassword)
      AuthService = strdup(DEFAULT_PAM_SERVICE);
    if (!AuthType)
      AuthType = strdup("Basic");

    if (!DocumentPrivacyScope)
      DocumentPrivacyScope = strdup(SERVER_SCOPE_DEFAULT);
    if (!DocumentPrivacyAttributes)
      DocumentPrivacyAttributes = strdup("default");

    if (!JobPrivacyScope)
      JobPrivacyScope = strdup(SERVER_SCOPE_DEFAULT);
    if (!JobPrivacyAttributes)
      JobPrivacyAttributes = strdup("default");

    if (!SubscriptionPrivacyScope)
      SubscriptionPrivacyScope = strdup(SERVER_SCOPE_DEFAULT);
    if (!SubscriptionPrivacyAttributes)
      SubscriptionPrivacyAttributes = strdup("default");
  }
  else
  {
    if (!DocumentPrivacyScope)
      DocumentPrivacyScope = strdup(SERVER_SCOPE_ALL);
    if (!DocumentPrivacyAttributes)
      DocumentPrivacyAttributes = strdup("none");

    if (!JobPrivacyScope)
      JobPrivacyScope = strdup(SERVER_SCOPE_ALL);
    if (!JobPrivacyAttributes)
      JobPrivacyAttributes = strdup("none");

    if (!SubscriptionPrivacyScope)
      SubscriptionPrivacyScope = strdup(SERVER_SCOPE_ALL);
    if (!SubscriptionPrivacyAttributes)
      SubscriptionPrivacyAttributes = strdup("none");
  }

  PrivacyAttributes = ippNew();

  add_document_privacy();
  add_job_privacy();
  add_subscription_privacy();

 /*
  * Initialize Bonjour...
  */

  serverDNSSDInit();

 /*
  * Apply default listeners if none are specified...
  */

  if (!Listeners)
  {
#ifdef WIN32
   /*
    * Windows is almost always used as a single user system, so use a default port
    * number of 8631.
    */

    if (!DefaultPort)
      DefaultPort = 8631;

#else
   /*
    * Use 8000 + UID mod 1000 for the default port number...
    */

    if (!DefaultPort)
      DefaultPort = 8000 + ((int)getuid() % 1000);
#endif /* WIN32 */

    serverLog(SERVER_LOGLEVEL_INFO, "Using default listeners for %s:%d.", ServerName, DefaultPort);

    if (!serverCreateListeners(strcmp(ServerName, "localhost") ? NULL : "localhost", DefaultPort))
      return (0);
  }

  return (1);
}


/*
 * 'serverFindPrinter()' - Find a printer by resource...
 */

server_printer_t *			/* O - Printer or NULL */
serverFindPrinter(const char *resource)	/* I - Resource path */
{
  server_printer_t	key,		/* Search key */
			*match = NULL;	/* Matching printer */


  _cupsMutexLock(&printer_mutex);
  if (cupsArrayCount(Printers) == 1 || !strcmp(resource, "/ipp/print"))
  {
   /*
    * Just use the first printer...
    */

    match = cupsArrayFirst(Printers);
    if (strcmp(match->resource, resource) && strcmp(resource, "/ipp/print"))
      match = NULL;
  }
  else
  {
    key.resource = (char *)resource;
    match        = (server_printer_t *)cupsArrayFind(Printers, &key);
  }
  _cupsMutexUnlock(&printer_mutex);

  return (match);
}


/*
 * 'serverLoadAttributes()' - Load printer attributes from a file.
 *
 * Syntax is based on ipptool format:
 *
 *    ATTR value-tag name value
 *    ATTR value-tag name value,value,...
 *    AUTHTYPE "scheme"
 *    COMMAND "/path/to/command"
 *    DEVICE-URI "uri"
 *    MAKE "manufacturer"
 *    MODEL "model name"
 *    PROXY-USER "username"
 *    STRINGS lang filename.strings
 *
 * AUTH schemes are "none" for no authentication or "basic" for HTTP Basic
 * authentication.
 *
 * DEVICE-URI values can be "socket", "ipp", or "ipps" URIs.
 */

int					/* O - 1 on success, 0 on failure */
serverLoadAttributes(
    const char     *filename,		/* I - File to load */
    server_pinfo_t *pinfo)		/* I - Printer information */
{
  _ipp_vars_t	vars;			/* IPP variables */


  _ippVarsInit(&vars, (_ipp_fattr_cb_t)attr_cb, (_ipp_ferror_cb_t)error_cb, (_ipp_ftoken_cb_t)token_cb);

  pinfo->attrs = _ippFileParse(&vars, filename, (void *)pinfo);

  _ippVarsDeinit(&vars);

  return (pinfo->attrs != NULL);
}


/*
 * 'serverLoadConfiguration()' - Load the server configuration file.
 */

int					/* O - 1 if successful, 0 on error */
serverLoadConfiguration(
    const char *directory)		/* I - Configuration directory */
{
  cups_dir_t	*dir;			/* Directory pointer */
  cups_dentry_t	*dent;			/* Directory entry */
  char		filename[1024],		/* Configuration file/directory */
                iconname[1024],		/* Icon file */
		resource[1024],		/* Resource path */
                *ptr;			/* Pointer into filename */
  server_printer_t *printer;		/* Printer */
  server_pinfo_t pinfo;			/* Printer information */


 /*
  * First read the system configuration file, if any...
  */

  snprintf(filename, sizeof(filename), "%s/system.conf", directory);
  if (!load_system(filename))
    return (0);

  if (!serverFinalizeConfiguration())
    return (0);

 /*
  * Then see if there are any print queues...
  */

  snprintf(filename, sizeof(filename), "%s/print", directory);
  if ((dir = cupsDirOpen(filename)) != NULL)
  {
    serverLog(SERVER_LOGLEVEL_INFO, "Loading printers from \"%s\".", filename);

    while ((dent = cupsDirRead(dir)) != NULL)
    {
      if ((ptr = dent->filename + strlen(dent->filename) - 5) >= dent->filename && !strcmp(ptr, ".conf"))
      {
       /*
        * Load the conf file, with any associated icon image.
        */

        serverLog(SERVER_LOGLEVEL_INFO, "Loading printer from \"%s\".", dent->filename);

        snprintf(filename, sizeof(filename), "%s/print/%s", directory, dent->filename);
        *ptr = '\0';

        memset(&pinfo, 0, sizeof(pinfo));
        pinfo.print_group = SERVER_GROUP_NONE;
	pinfo.proxy_group = SERVER_GROUP_NONE;

        snprintf(iconname, sizeof(iconname), "%s/print/%s.png", directory, dent->filename);
        if (!access(iconname, R_OK))
          pinfo.icon = strdup(iconname);

        if (serverLoadAttributes(filename, &pinfo))
	{
          snprintf(resource, sizeof(resource), "/ipp/print/%s", dent->filename);

	  if ((printer = serverCreatePrinter(resource, dent->filename, &pinfo)) == NULL)
            continue;

	  if (!Printers)
	    Printers = cupsArrayNew((cups_array_func_t)compare_printers, NULL);

	  cupsArrayAdd(Printers, printer);
	}
      }
      else if (!strstr(dent->filename, ".png"))
        serverLog(SERVER_LOGLEVEL_INFO, "Skipping \"%s\".", dent->filename);
    }

    cupsDirClose(dir);
  }

 /*
  * Finally, see if there are any 3D print queues...
  */

  snprintf(filename, sizeof(filename), "%s/print3d", directory);
  if ((dir = cupsDirOpen(filename)) != NULL)
  {
    serverLog(SERVER_LOGLEVEL_INFO, "Loading 3D printers from \"%s\".", filename);

    while ((dent = cupsDirRead(dir)) != NULL)
    {
      if ((ptr = dent->filename + strlen(dent->filename) - 5) >= dent->filename && !strcmp(ptr, ".conf"))
      {
       /*
        * Load the conf file, with any associated icon image.
        */

        serverLog(SERVER_LOGLEVEL_INFO, "Loading 3D printer from \"%s\".", dent->filename);

        snprintf(filename, sizeof(filename), "%s/print3d/%s", directory, dent->filename);
        *ptr = '\0';

        memset(&pinfo, 0, sizeof(pinfo));
        pinfo.print_group = SERVER_GROUP_NONE;
	pinfo.proxy_group = SERVER_GROUP_NONE;

        snprintf(iconname, sizeof(iconname), "%s/print3d/%s.png", directory, dent->filename);
        if (!access(iconname, R_OK))
          pinfo.icon = strdup(iconname);

        if (serverLoadAttributes(filename, &pinfo))
	{
          snprintf(resource, sizeof(resource), "/ipp/print3d/%s", dent->filename);

	  if ((printer = serverCreatePrinter(resource, dent->filename, &pinfo)) == NULL)
          continue;

	  if (!Printers)
	    Printers = cupsArrayNew((cups_array_func_t)compare_printers, NULL);

	  cupsArrayAdd(Printers, printer);
	}
      }
      else if (!strstr(dent->filename, ".png"))
        serverLog(SERVER_LOGLEVEL_INFO, "Skipping \"%s\".", dent->filename);
    }

    cupsDirClose(dir);
  }

  return (1);
}


/*
 * 'add_document_privacy()' - Add document privacy attributes.
 */

static void
add_document_privacy(void)
{
  int		i;			/* Looping var */
  char		temp[1024],		/* Temporary copy of value */
		*start,			/* Start of value */
		*ptr;			/* Pointer into value */
  ipp_attribute_t *privattrs = NULL;	/* document-privacy-attributes */
  static const char * const description[] =
  {					/* document-description attributes */
    "compression",
    "copies-actual",
    "cover-back-actual",
    "cover-front-actual",
    "current-page-order",
    "date-time-at-completed",
    "date-time-at-creation",
    "date-time-at-processing",
    "detailed-status-messages",
    "document-access-errors",
    "document-charset",
    "document-digital-signature",
    "document-format",
    "document-format-details",
    "document-format-detected",
    "document-format-version",
    "document-format-version-detected",
    "document-message",
    "document-metadata",
    "document-name",
    "document-natural-language",
    "document-state",
    "document-state-message",
    "document-state-reasons",
    "document-uri",
    "errors-count",
    "finishings-actual",
    "finishings-col-actual",
    "force-front-side-actual",
    "imposition-template-actual",
    "impressions",
    "impressions-col",
    "impressions-completed",
    "impressions-completed-col",
    "impressions-completed-current-copy",
    "insert-sheet-actual",
    "k-octets",
    "k-octets-processed",
    "last-document",
    "materials-col-actual",
    "media-actual",
    "media-col-actual",
    "media-input-tray-check-actual",
    "media-sheets",
    "media-sheets-col",
    "media-sheets-completed",
    "media-sheets-completed-col",
    "more-info",
    "multiple-object-handling-actual",
    "number-up-actual",
    "orientation-requested-actual",
    "output-bin-actual",
    "output-device-assigned",
    "overrides-actual",
    "page-delivery-actual",
    "page-order-received-actual",
    "page-ranges-actual",
    "pages",
    "pages-col",
    "pages-completed",
    "pages-completed-col",
    "pages-completed-current-copy",
    "platform-temperature-actual",
    "presentation-direction-number-up-actual",
    "print-accuracy-actual",
    "print-base-actual",
    "print-color-mode-actual",
    "print-content-optimize-actual",
    "print-objects-actual",
    "print-quality-actual",
    "print-rendering-intent-actual",
    "print-scaling-actual",
    "print-supports-actual",
    "printer-resolution-actual",
    "printer-up-time",
    "separator-sheets-actual",
    "sheet-completed-copy-number",
    "sides-actual",
    "time-at-completed",
    "time-at-creation",
    "time-at-processing",
    "x-image-position-actual",
    "x-image-shift-actual",
    "x-side1-image-shift-actual",
    "x-side2-image-shift-actual",
    "y-image-position-actual",
    "y-image-shift-actual",
    "y-side1-image-shift-actual",
    "y-side2-image-shift-actual"
  };
  static const char * const template[] =
  {					/* document-template attributes */
    "copies",
    "cover-back",
    "cover-front",
    "feed-orientation",
    "finishings",
    "finishings-col",
    "font-name-requested",
    "font-size-requested",
    "force-front-side",
    "imposition-template",
    "insert-sheet",
    "materials-col",
    "media",
    "media-col",
    "media-input-tray-check",
    "multiple-document-handling",
    "multiple-object-handling",
    "number-up",
    "orientation-requested",
    "overrides",
    "page-delivery",
    "page-order-received",
    "page-ranges",
    "pages-per-subset",
    "pdl-init-file",
    "platform-temperature",
    "presentation-direction-number-up",
    "print-accuracy",
    "print-base",
    "print-color-mode",
    "print-content-optimize",
    "print-objects",
    "print-quality",
    "print-rendering-intent",
    "print-scaling",
    "print-supports",
    "printer-resolution",
    "separator-sheets",
    "sheet-collate",
    "sides",
    "x-image-position",
    "x-image-shift",
    "x-side1-image-shift",
    "x-side2-image-shift",
    "y-image-position",
    "y-image-shift",
    "y-side1-image-shift",
    "y-side2-image-shift"
  };


  if (!strcmp(DocumentPrivacyAttributes, "none"))
  {
    ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "document-privacy-attributes", NULL, "none");
  }
  else if (!strcmp(DocumentPrivacyAttributes, "all"))
  {
    ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "document-privacy-attributes", NULL, "all");

    DocumentPrivacyArray = cupsArrayNew3((cups_array_func_t)strcmp, NULL, NULL, 0, (cups_acopy_func_t)strdup, (cups_afree_func_t)free);
    for (i = 0; i < (int)(sizeof(description) / sizeof(description[0])); i ++)
      cupsArrayAdd(DocumentPrivacyArray, (void *)description[i]);
    for (i = 0; i < (int)(sizeof(template) / sizeof(template[0])); i ++)
      cupsArrayAdd(DocumentPrivacyArray, (void *)template[i]);
  }
  else
  {
    DocumentPrivacyArray = cupsArrayNew3((cups_array_func_t)strcmp, NULL, NULL, 0, (cups_acopy_func_t)strdup, (cups_afree_func_t)free);

    strlcpy(temp, DocumentPrivacyAttributes, sizeof(temp));

    ptr = temp;
    while (*ptr)
    {
      start = ptr;
      while (*ptr && *ptr != ',')
	ptr ++;
      if (*ptr == ',')
	*ptr++ = '\0';

      if (!strcmp(start, "all") || !strcmp(start, "none"))
	continue;

      if (!privattrs)
	privattrs = ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "document-privacy-attributes", NULL, start);
      else
	ippSetString(PrivacyAttributes, &privattrs, ippGetCount(privattrs), start);

      if (!strcmp(start, "default"))
      {
	for (i = 0; i < (int)(sizeof(description) / sizeof(description[0])); i ++)
	  cupsArrayAdd(DocumentPrivacyArray, (void *)description[i]);
	for (i = 0; i < (int)(sizeof(template) / sizeof(template[0])); i ++)
	  cupsArrayAdd(DocumentPrivacyArray, (void *)template[i]);
      }
      else if (!strcmp(start, "document-description"))
      {
	for (i = 0; i < (int)(sizeof(description) / sizeof(description[0])); i ++)
	  cupsArrayAdd(DocumentPrivacyArray, (void *)description[i]);
      }
      else if (!strcmp(start, "document-template"))
      {
	for (i = 0; i < (int)(sizeof(template) / sizeof(template[0])); i ++)
	  cupsArrayAdd(DocumentPrivacyArray, (void *)template[i]);
      }
      else
      {
	cupsArrayAdd(DocumentPrivacyArray, (void *)start);
      }
    }
  }

  ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "document-privacy-scope", NULL, DocumentPrivacyScope);
}


/*
 * 'add_job_privacy()' - Add job privacy attributes.
 */

static void
add_job_privacy(void)
{
  int		i;			/* Looping var */
  char		temp[1024],		/* Temporary copy of value */
		*start,			/* Start of value */
		*ptr;			/* Pointer into value */
  ipp_attribute_t *privattrs = NULL;	/* job-privacy-attributes */
  static const char * const description[] =
  {					/* job-description attributes */
    "compression-supplied",
    "copies-actual",
    "cover-back-actual",
    "cover-front-actual",
    "current-page-order",
    "date-time-at-completed",
    "date-time-at-creation",
    "date-time-at-processing",
    "destination-statuses",
    "document-charset-supplied",
    "document-digital-signature-supplied",
    "document-format-details-supplied",
    "document-format-supplied",
    "document-message-supplied",
    "document-metadata",
    "document-name-supplied",
    "document-natural-language-supplied",
    "document-overrides-actual",
    "errors-count",
    "finishings-actual",
    "finishings-col-actual",
    "force-front-side-actual",
    "imposition-template-actual",
    "impressions-completed-current-copy",
    "insert-sheet-actual",
    "job-account-id-actual",
    "job-accounting-sheets-actual",
    "job-accounting-user-id-actual",
    "job-attribute-fidelity",
    "job-collation-type",
    "job-collation-type-actual",
    "job-copies-actual",
    "job-cover-back-actual",
    "job-cover-front-actual",
    "job-detailed-status-message",
    "job-document-access-errors",
    "job-error-sheet-actual",
    "job-finishings-actual",
    "job-finishings-col-actual",
    "job-hold-until-actual",
    "job-impressions",
    "job-impressions-col",
    "job-impressions-completed",
    "job-impressions-completed-col",
    "job-k-octets",
    "job-k-octets-processed",
    "job-mandatory-attributes",
    "job-media-sheets",
    "job-media-sheets-col",
    "job-media-sheets-completed",
    "job-media-sheets-completed-col",
    "job-message-from-operator",
    "job-more-info",
    "job-name",
    "job-originating-user-name",
    "job-originating-user-uri",
    "job-pages",
    "job-pages-col",
    "job-pages-completed",
    "job-pages-completed-col",
    "job-pages-completed-current-copy",
    "job-priority-actual",
    "job-save-printer-make-and-model",
    "job-sheet-message-actual",
    "job-sheets-actual",
    "job-sheets-col-actual",
    "job-state",
    "job-state-message",
    "job-state-reasons",
    "materials-col-actual",
    "media-actual",
    "media-col-actual",
    "media-check-input-tray-actual",
    "multiple-document-handling-actual",
    "multiple-object-handling-actual",
    "number-of-documents",
    "number-of-intervening-jobs",
    "number-up-actual",
    "orientation-requested-actual",
    "original-requesting-user-name",
    "output-bin-actual",
    "output-device-assigned",
    "overrides-actual",
    "page-delivery-actual",
    "page-order-received-actual",
    "page-ranges-actual",
    "platform-temperature-actual",
    "presentation-direction-number-up-actual",
    "print-accuracy-actual",
    "print-base-actual",
    "print-color-mode-actual",
    "print-content-optimize-actual",
    "print-objects-actual",
    "print-quality-actual",
    "print-rendering-intent-actual",
    "print-scaling-actual",
    "print-supports-actual",
    "printer-resolution-actual",
    "separator-sheets-actual",
    "sheet-collate-actual",
    "sheet-completed-copy-number",
    "sheet-completed-document-number",
    "sides-actual",
    "time-at-completed",
    "time-at-creation",
    "time-at-processing",
    "warnings-count",
    "x-image-position-actual",
    "x-image-shift-actual",
    "x-side1-image-shift-actual",
    "x-side2-image-shift-actual",
    "y-image-position-actual",
    "y-image-shift-actual",
    "y-side1-image-shift-actual",
    "y-side2-image-shift-actual"
  };
  static const char * const template[] =
  {					/* job-template attributes */
    "confirmation-sheet-print",
    "copies",
    "cover-back",
    "cover-front",
    "cover-sheet-info",
    "destination-uris",
    "feed-orientation",
    "finishings",
    "finishings-col",
    "font-name-requested",
    "font-size-requested",
    "force-front-side",
    "imposition-template",
    "insert-sheet",
    "job-account-id",
    "job-accounting-sheets"
    "job-accounting-user-id",
    "job-copies",
    "job-cover-back",
    "job-cover-front",
    "job-delay-output-until",
    "job-delay-output-until-time",
    "job-error-action",
    "job-error-sheet",
    "job-finishings",
    "job-finishings-col",
    "job-hold-until",
    "job-hold-until-time",
    "job-message-to-operator",
    "job-phone-number",
    "job-priority",
    "job-recipient-name",
    "job-save-disposition",
    "job-sheets",
    "job-sheets-col",
    "materials-col",
    "media",
    "media-col",
    "media-input-tray-check",
    "multiple-document-handling",
    "multiple-object-handling",
    "number-of-retries",
    "number-up",
    "orientation-requested",
    "output-bin",
    "output-device",
    "overrides",
    "page-delivery",
    "page-order-received",
    "page-ranges",
    "pages-per-subset",
    "pdl-init-file",
    "platform-temperature",
    "presentation-direction-number-up",
    "print-accuracy",
    "print-base",
    "print-color-mode",
    "print-content-optimize",
    "print-objects",
    "print-quality",
    "print-rendering-intent",
    "print-scaling",
    "print-supports",
    "printer-resolution",
    "proof-print",
    "retry-interval",
    "retry-timeout",
    "separator-sheets",
    "sheet-collate",
    "sides",
    "x-image-position",
    "x-image-shift",
    "x-side1-image-shift",
    "x-side2-image-shift",
    "y-image-position",
    "y-image-shift",
    "y-side1-image-shift",
    "y-side2-image-shift",
  };


  if (!strcmp(JobPrivacyAttributes, "none"))
  {
    ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "job-privacy-attributes", NULL, "none");
  }
  else if (!strcmp(JobPrivacyAttributes, "all"))
  {
    ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "job-privacy-attributes", NULL, "all");

    JobPrivacyArray = cupsArrayNew3((cups_array_func_t)strcmp, NULL, NULL, 0, (cups_acopy_func_t)strdup, (cups_afree_func_t)free);
    for (i = 0; i < (int)(sizeof(description) / sizeof(description[0])); i ++)
      cupsArrayAdd(JobPrivacyArray, (void *)description[i]);
    for (i = 0; i < (int)(sizeof(template) / sizeof(template[0])); i ++)
      cupsArrayAdd(JobPrivacyArray, (void *)template[i]);
  }
  else
  {
    JobPrivacyArray = cupsArrayNew3((cups_array_func_t)strcmp, NULL, NULL, 0, (cups_acopy_func_t)strdup, (cups_afree_func_t)free);

    strlcpy(temp, JobPrivacyAttributes, sizeof(temp));

    ptr = temp;
    while (*ptr)
    {
      start = ptr;
      while (*ptr && *ptr != ',')
	ptr ++;
      if (*ptr == ',')
	*ptr++ = '\0';

      if (!strcmp(start, "all") || !strcmp(start, "none"))
	continue;

      if (!privattrs)
	privattrs = ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "job-privacy-attributes", NULL, start);
      else
	ippSetString(PrivacyAttributes, &privattrs, ippGetCount(privattrs), start);

      if (!strcmp(start, "default"))
      {
	for (i = 0; i < (int)(sizeof(description) / sizeof(description[0])); i ++)
	  cupsArrayAdd(JobPrivacyArray, (void *)description[i]);
	for (i = 0; i < (int)(sizeof(template) / sizeof(template[0])); i ++)
	  cupsArrayAdd(JobPrivacyArray, (void *)template[i]);
      }
      else if (!strcmp(start, "job-description"))
      {
	for (i = 0; i < (int)(sizeof(description) / sizeof(description[0])); i ++)
	  cupsArrayAdd(JobPrivacyArray, (void *)description[i]);
      }
      else if (!strcmp(start, "job-template"))
      {
	for (i = 0; i < (int)(sizeof(template) / sizeof(template[0])); i ++)
	  cupsArrayAdd(JobPrivacyArray, (void *)template[i]);
      }
      else
      {
	cupsArrayAdd(JobPrivacyArray, (void *)start);
      }
    }
  }

  ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "job-privacy-scope", NULL, JobPrivacyScope);
}


/*
 * 'add_subscription_privacy()' - Add subscription privacy attributes.
 */

static void
add_subscription_privacy(void)
{
  int		i;			/* Looping var */
  char		temp[1024],		/* Temporary copy of value */
		*start,			/* Start of value */
		*ptr;			/* Pointer into value */
  ipp_attribute_t *privattrs = NULL;	/* job-privacy-attributes */
  static const char * const description[] =
  {					/* subscription-description attributes */
    "notify-lease-expiration-time",
    "notify-sequence-number",
    "notify-subscriber-user-name"
  };
  static const char * const template[] =
  {					/* subscription-template attributes */
    "notify-attributes",
    "notify-charset",
    "notify-events",
    "notify-lease-duration",
    "notify-natural-language",
    "notify-pull-method",
    "notify-recipient-uri",
    "notify-time-interval",
    "notify-user-data"
  };


  if (!strcmp(SubscriptionPrivacyAttributes, "none"))
  {
    ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "subscription-privacy-attributes", NULL, "none");
  }
  else if (!strcmp(SubscriptionPrivacyAttributes, "all"))
  {
    ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "subscription-privacy-attributes", NULL, "all");

    SubscriptionPrivacyArray = cupsArrayNew3((cups_array_func_t)strcmp, NULL, NULL, 0, (cups_acopy_func_t)strdup, (cups_afree_func_t)free);
    for (i = 0; i < (int)(sizeof(description) / sizeof(description[0])); i ++)
      cupsArrayAdd(SubscriptionPrivacyArray, (void *)description[i]);
    for (i = 0; i < (int)(sizeof(template) / sizeof(template[0])); i ++)
      cupsArrayAdd(SubscriptionPrivacyArray, (void *)template[i]);
  }
  else
  {
    SubscriptionPrivacyArray = cupsArrayNew3((cups_array_func_t)strcmp, NULL, NULL, 0, (cups_acopy_func_t)strdup, (cups_afree_func_t)free);

    strlcpy(temp, SubscriptionPrivacyAttributes, sizeof(temp));

    ptr = temp;
    while (*ptr)
    {
      start = ptr;
      while (*ptr && *ptr != ',')
	ptr ++;
      if (*ptr == ',')
	*ptr++ = '\0';

      if (!strcmp(start, "all") || !strcmp(start, "none"))
	continue;

      if (!privattrs)
	privattrs = ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "subscription-privacy-attributes", NULL, start);
      else
	ippSetString(PrivacyAttributes, &privattrs, ippGetCount(privattrs), start);

      if (!strcmp(start, "default"))
      {
	for (i = 0; i < (int)(sizeof(description) / sizeof(description[0])); i ++)
	  cupsArrayAdd(SubscriptionPrivacyArray, (void *)description[i]);
	for (i = 0; i < (int)(sizeof(template) / sizeof(template[0])); i ++)
	  cupsArrayAdd(SubscriptionPrivacyArray, (void *)template[i]);
      }
      else if (!strcmp(start, "subscription-description"))
      {
	for (i = 0; i < (int)(sizeof(description) / sizeof(description[0])); i ++)
	  cupsArrayAdd(SubscriptionPrivacyArray, (void *)description[i]);
      }
      else if (!strcmp(start, "subscription-template"))
      {
	for (i = 0; i < (int)(sizeof(template) / sizeof(template[0])); i ++)
	  cupsArrayAdd(SubscriptionPrivacyArray, (void *)template[i]);
      }
      else
      {
	cupsArrayAdd(SubscriptionPrivacyArray, (void *)start);
      }
    }
  }

  ippAddString(PrivacyAttributes, IPP_TAG_PRINTER, IPP_CONST_TAG(IPP_TAG_KEYWORD), "subscription-privacy-scope", NULL, SubscriptionPrivacyScope);
}


/*
 * 'attr_cb()' - Determine whether an attribute should be loaded.
 */

static int				/* O - 1 to use, 0 to ignore */
attr_cb(_ipp_file_t    *f,		/* I - IPP file */
        server_pinfo_t *pinfo,		/* I - Printer information */
        const char     *attr)		/* I - Attribute name */
{
  int		i,			/* Current element */
		result;			/* Result of comparison */
  static const char * const ignored[] =
  {					/* Ignored attributes */
    "attributes-charset",
    "attributes-natural-language",
    "charset-configured",
    "charset-supported",
    "device-service-count",
    "device-uuid",
    "document-format-varying-attributes",
    "job-settable-attributes-supported",
    "printer-alert",
    "printer-alert-description",
    "printer-camera-image-uri",
    "printer-charge-info",
    "printer-charge-info-uri",
    "printer-config-change-date-time",
    "printer-config-change-time",
    "printer-current-time",
    "printer-detailed-status-messages",
    "printer-dns-sd-name",
    "printer-fax-log-uri",
    "printer-get-attributes-supported",
    "printer-icons",
    "printer-id",
    "printer-is-accepting-jobs",
    "printer-message-date-time",
    "printer-message-from-operator",
    "printer-message-time",
    "printer-more-info",
    "printer-service-type",
    "printer-settable-attributes-supported",
    "printer-state",
    "printer-state-message",
    "printer-state-reasons",
    "printer-static-resource-directory-uri",
    "printer-static-resource-k-octets-free",
    "printer-static-resource-k-octets-supported",
    "printer-strings-languages-supported",
    "printer-strings-uri",
    "printer-supply-info-uri",
    "printer-up-time",
    "printer-uri-supported",
    "printer-xri-supported",
    "queued-job-count",
    "uri-authentication-supported",
    "uri-security-supported",
    "xri-authentication-supported",
    "xri-security-supported",
    "xri-uri-scheme-supported"
  };


  (void)f;
  (void)pinfo;

  for (i = 0, result = 1; i < (int)(sizeof(ignored) / sizeof(ignored[0])); i ++)
  {
    if ((result = strcmp(attr, ignored[i])) <= 0)
      break;
  }

  return (result != 0);
}


/*
 * 'compare_lang()' - Compare two languages.
 */

static int				/* O - Result of comparison */
compare_lang(server_lang_t *a,		/* I - First localization */
             server_lang_t *b)		/* I - Second localization */
{
  return (strcmp(a->lang, b->lang));
}


/*
 * 'compare_printers()' - Compare two printers.
 */

static int				/* O - Result of comparison */
compare_printers(server_printer_t *a,	/* I - First printer */
                 server_printer_t *b)	/* I - Second printer */
{
  return (strcmp(a->resource, b->resource));
}


/*
 * 'copy_lang()' - Copy a localization.
 */

static server_lang_t *			/* O - New localization */
copy_lang(server_lang_t *a)		/* I - Localization to copy */
{
  server_lang_t	*b;			/* New localization */


  if ((b = calloc(1, sizeof(server_lang_t))) != NULL)
  {
    b->lang     = strdup(a->lang);
    b->filename = strdup(a->filename);
  }

  return (b);
}


#ifdef HAVE_AVAHI
/*
 * 'dnssd_client_cb()' - Client callback for Avahi.
 *
 * Called whenever the client or server state changes...
 */

static void
dnssd_client_cb(
    AvahiClient      *c,		/* I - Client */
    AvahiClientState state,		/* I - Current state */
    void             *userdata)		/* I - User data (unused) */
{
  (void)userdata;

  if (!c)
    return;

  switch (state)
  {
    default :
        fprintf(stderr, "Ignore Avahi state %d.\n", state);
	break;

    case AVAHI_CLIENT_FAILURE:
	if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED)
	{
	  fputs("Avahi server crashed, exiting.\n", stderr);
	  exit(1);
	}
	break;
  }
}
#endif /* HAVE_AVAHI */


/*
 * 'error_cb()' - Log an error message.
 */

static int				/* O - 1 to continue, 0 to stop */
error_cb(_ipp_file_t    *f,		/* I - IPP file data */
         server_pinfo_t *pinfo,		/* I - Printer information */
         const char     *error)		/* I - Error message */
{
  (void)f;
  (void)pinfo;

  serverLog(SERVER_LOGLEVEL_ERROR, "%s", error);

  return (1);
}


/*
 * 'free_lang()' - Free a localization.
 */

static void
free_lang(server_lang_t *a)		/* I - Localization */
{
  free(a->lang);
  free(a->filename);
  free(a);
}


/*
 * 'load_system()' - Load the system configuration file.
 */

static int				/* O - 1 on success, 0 on failure */
load_system(const char *conf)		/* I - Configuration file */
{
  cups_file_t	*fp;			/* File pointer */
  int		status = 1,		/* Return value */
		linenum = 0;		/* Current line number */
  char		line[1024],		/* Line from file */
		*value;			/* Pointer to value on line */
  struct group	*group;			/* Group information */


  if ((fp = cupsFileOpen(conf, "r")) == NULL)
    return (errno == ENOENT);

  while (cupsFileGetConf(fp, line, sizeof(line), &value, &linenum))
  {
    if (!value)
    {
      fprintf(stderr, "ippserver: Missing value on line %d of \"%s\".\n", linenum, conf);
      status = 0;
      break;
    }

    if (!_cups_strcasecmp(line, "Authentication"))
    {
      if (!_cups_strcasecmp(value, "on") || !_cups_strcasecmp(value, "yes"))
      {
        Authentication = 1;
      }
      else if (!_cups_strcasecmp(value, "off") || !_cups_strcasecmp(value, "no"))
      {
        Authentication = 0;
      }
      else
      {
        fprintf(stderr, "ippserver: Unknown Authentication \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }
    }
    else if (!_cups_strcasecmp(line, "AuthAdminGroup"))
    {
      if ((group = getgrnam(value)) == NULL)
      {
        fprintf(stderr, "ippserver: Unable to find AuthAdminGroup \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }

      AuthAdminGroup = group->gr_gid;
    }
    else if (!_cups_strcasecmp(line, "AuthName"))
    {
      AuthName = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "AuthOperatorGroup"))
    {
      if ((group = getgrnam(value)) == NULL)
      {
        fprintf(stderr, "ippserver: Unable to find AuthOperatorGroup \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }

      AuthOperatorGroup = group->gr_gid;
    }
    else if (!_cups_strcasecmp(line, "AuthService"))
    {
      AuthService = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "AuthTestPassword"))
    {
      AuthTestPassword = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "AuthType"))
    {
      AuthType = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "DataDirectory"))
    {
      if (access(value, R_OK))
      {
        fprintf(stderr, "ippserver: Unable to access DataDirectory \"%s\": %s\n", value, strerror(errno));
        status = 0;
        break;
      }

      DataDirectory = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "DefaultPrinter"))
    {
      if (DefaultPrinter)
      {
        fprintf(stderr, "ippserver: Extra DefaultPrinter seen on line %d of \"%s\".\n", linenum, conf);
        status = 0;
        break;
      }

      DefaultPrinter = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "DocumentPrivacyAttributes"))
    {
      if (DocumentPrivacyAttributes)
      {
        fprintf(stderr, "ippserver: Extra DocumentPrivacyAttributes seen on line %d of \"%s\".\n", linenum, conf);
        status = 0;
        break;
      }

      DocumentPrivacyAttributes = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "DocumentPrivacyScope"))
    {
      if (DocumentPrivacyScope)
      {
        fprintf(stderr, "ippserver: Extra DocumentPrivacyScope seen on line %d of \"%s\".\n", linenum, conf);
        status = 0;
        break;
      }

      DocumentPrivacyScope = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "Encryption"))
    {
      if (!_cups_strcasecmp(value, "always"))
        Encryption = HTTP_ENCRYPTION_ALWAYS;
      else if (!_cups_strcasecmp(value, "ifrequested"))
        Encryption = HTTP_ENCRYPTION_IF_REQUESTED;
      else if (!_cups_strcasecmp(value, "never"))
        Encryption = HTTP_ENCRYPTION_NEVER;
      else if (!_cups_strcasecmp(value, "required"))
        Encryption = HTTP_ENCRYPTION_REQUIRED;
      else
      {
        fprintf(stderr, "ippserver: Bad Encryption value \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }
    }
    else if (!_cups_strcasecmp(line, "JobPrivacyAttributes"))
    {
      if (JobPrivacyAttributes)
      {
        fprintf(stderr, "ippserver: Extra JobPrivacyAttributes seen on line %d of \"%s\".\n", linenum, conf);
        status = 0;
        break;
      }

      JobPrivacyAttributes = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "JobPrivacyScope"))
    {
      if (JobPrivacyScope)
      {
        fprintf(stderr, "ippserver: Extra JobPrivacyScope seen on line %d of \"%s\".\n", linenum, conf);
        status = 0;
        break;
      }

      JobPrivacyScope = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "KeepFiles"))
    {
      KeepFiles = !strcasecmp(value, "yes") || !strcasecmp(value, "true") || !strcasecmp(value, "on");
    }
    else if (!_cups_strcasecmp(line, "Listen"))
    {
      char	*ptr;			/* Pointer into host value */
      int	port;			/* Port number */

      if ((ptr = strrchr(value, ':')) != NULL && !isdigit(ptr[1] & 255))
      {
        fprintf(stderr, "ippserver: Bad Listen value \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }

      if (ptr)
      {
        *ptr++ = '\0';
        port   = atoi(ptr);
      }
      else
        port = 8000 + ((int)getuid() % 1000);

      if (!serverCreateListeners(value, port))
      {
        status = 0;
        break;
      }
    }
    else if (!_cups_strcasecmp(line, "LogFile"))
    {
      if (!_cups_strcasecmp(value, "stderr"))
        LogFile = NULL;
      else
        LogFile = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "LogLevel"))
    {
      if (!_cups_strcasecmp(value, "error"))
        LogLevel = SERVER_LOGLEVEL_ERROR;
      else if (!_cups_strcasecmp(value, "info"))
        LogLevel = SERVER_LOGLEVEL_INFO;
      else if (!_cups_strcasecmp(value, "debug"))
        LogLevel = SERVER_LOGLEVEL_DEBUG;
      else
      {
        fprintf(stderr, "ippserver: Bad LogLevel value \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }
    }
    else if (!_cups_strcasecmp(line, "MaxCompletedJobs"))
    {
      if (!isdigit(*value & 255))
      {
        fprintf(stderr, "ippserver: Bad MaxCompletedJobs value \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }

      MaxCompletedJobs = atoi(value);
    }
    else if (!_cups_strcasecmp(line, "MaxJobs"))
    {
      if (!isdigit(*value & 255))
      {
        fprintf(stderr, "ippserver: Bad MaxJobs value \"%s\" on line %d of \"%s\".\n", value, linenum, conf);
        status = 0;
        break;
      }

      MaxJobs = atoi(value);
    }
    else if (!_cups_strcasecmp(line, "SpoolDirectory"))
    {
      if (access(value, R_OK))
      {
        fprintf(stderr, "ippserver: Unable to access SpoolDirectory \"%s\": %s\n", value, strerror(errno));
        status = 0;
        break;
      }

      SpoolDirectory = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "SubscriptionPrivacyAttributes"))
    {
      if (SubscriptionPrivacyAttributes)
      {
        fprintf(stderr, "ippserver: Extra SubscriptionPrivacyAttributes seen on line %d of \"%s\".\n", linenum, conf);
        status = 0;
        break;
      }

      SubscriptionPrivacyAttributes = strdup(value);
    }
    else if (!_cups_strcasecmp(line, "SubscriptionPrivacyScope"))
    {
      if (SubscriptionPrivacyScope)
      {
        fprintf(stderr, "ippserver: Extra SubscriptionPrivacyScope seen on line %d of \"%s\".\n", linenum, conf);
        status = 0;
        break;
      }

      SubscriptionPrivacyScope = strdup(value);
    }
    else
    {
      fprintf(stderr, "ippserver: Unknown directive \"%s\" on line %d.\n", line, linenum);
    }
  }

  cupsFileClose(fp);

  return (status);
}


/*
 * 'token_cb()' - Process ippserver-specific config file tokens.
 */

static int				/* O - 1 to continue, 0 to stop */
token_cb(_ipp_file_t    *f,		/* I - IPP file data */
         _ipp_vars_t    *vars,		/* I - IPP variables */
         server_pinfo_t *pinfo,		/* I - Printer information */
         const char     *token)		/* I - Current token */
{
  char	temp[1024],			/* Temporary string */
	value[1024];			/* Value string */


  if (!token)
  {
   /*
    * NULL token means do the initial setup - create an empty IPP message and
    * return...
    */

    f->attrs = ippNew();

    return (1);
  }
  else if (!_cups_strcasecmp(token, "AuthPrintGroup"))
  {
    struct group	*group;		/* Group information */

    if (!_ippFileReadToken(f, temp, sizeof(temp)))
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Missing AuthPrintGroup value on line %d of \"%s\".", f->linenum, f->filename);
      return (0);
    }

    _ippVarsExpand(vars, value, temp, sizeof(value));

    if ((group = getgrnam(value)) == NULL)
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Unknown AuthPrintGroup \"%s\" on line %d of \"%s\".", value, f->linenum, f->filename);
      return (0);
    }

    pinfo->print_group = group->gr_gid;
  }
  else if (!_cups_strcasecmp(token, "AuthProxyGroup"))
  {
    struct group	*group;		/* Group information */

    if (!_ippFileReadToken(f, temp, sizeof(temp)))
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Missing AuthProxyGroup value on line %d of \"%s\".", f->linenum, f->filename);
      return (0);
    }

    _ippVarsExpand(vars, value, temp, sizeof(value));

    if ((group = getgrnam(value)) == NULL)
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Unknown AuthProxyGroup \"%s\" on line %d of \"%s\".", value, f->linenum, f->filename);
      return (0);
    }

    pinfo->proxy_group = group->gr_gid;
  }
  else if (!_cups_strcasecmp(token, "Command"))
  {
    if (!_ippFileReadToken(f, temp, sizeof(temp)))
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Missing Command value on line %d of \"%s\".", f->linenum, f->filename);
      return (0);
    }

    _ippVarsExpand(vars, value, temp, sizeof(value));

    pinfo->command = strdup(value);
  }
  else if (!_cups_strcasecmp(token, "DeviceURI"))
  {
    if (!_ippFileReadToken(f, temp, sizeof(temp)))
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Missing DeviceURI value on line %d of \"%s\".", f->linenum, f->filename);
      return (0);
    }

    _ippVarsExpand(vars, value, temp, sizeof(value));

    pinfo->device_uri = strdup(value);
  }
  else if (!_cups_strcasecmp(token, "OutputFormat"))
  {
    if (!_ippFileReadToken(f, temp, sizeof(temp)))
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Missing OutputFormat value on line %d of \"%s\".", f->linenum, f->filename);
      return (0);
    }

    _ippVarsExpand(vars, value, temp, sizeof(value));

    pinfo->output_format = strdup(value);
  }
  else if (!_cups_strcasecmp(token, "Make"))
  {
    if (!_ippFileReadToken(f, temp, sizeof(temp)))
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Missing Make value on line %d of \"%s\".", f->linenum, f->filename);
      return (0);
    }

    _ippVarsExpand(vars, value, temp, sizeof(value));

    pinfo->make = strdup(value);
  }
  else if (!_cups_strcasecmp(token, "Model"))
  {
    if (!_ippFileReadToken(f, temp, sizeof(temp)))
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Missing Model value on line %d of \"%s\".", f->linenum, f->filename);
      return (0);
    }

    _ippVarsExpand(vars, value, temp, sizeof(value));

    pinfo->model = strdup(value);
  }
  else if (!_cups_strcasecmp(token, "Strings"))
  {
    server_lang_t	lang;			/* New localization */
    char		stringsfile[1024];	/* Strings filename */

    if (!_ippFileReadToken(f, temp, sizeof(temp)))
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Missing STRINGS language on line %d of \"%s\".", f->linenum, f->filename);
      return (0);
    }

    _ippVarsExpand(vars, value, temp, sizeof(value));

    if (!_ippFileReadToken(f, temp, sizeof(temp)))
    {
      serverLog(SERVER_LOGLEVEL_ERROR, "Missing STRINGS filename on line %d of \"%s\".", f->linenum, f->filename);
      return (0);
    }

    _ippVarsExpand(vars, stringsfile, temp, sizeof(stringsfile));

    lang.lang     = value;
    lang.filename = stringsfile;

    if (!pinfo->strings)
      pinfo->strings = cupsArrayNew3((cups_array_func_t)compare_lang, NULL, NULL, 0, (cups_acopy_func_t)copy_lang, (cups_afree_func_t)free_lang);

    cupsArrayAdd(pinfo->strings, &lang);

    serverLog(SERVER_LOGLEVEL_DEBUG, "Added strings file \"%s\" for language \"%s\".", stringsfile, value);
  }
  else
  {
    serverLog(SERVER_LOGLEVEL_ERROR, "Unknown directive \"%s\" on line %d of \"%s\".", token, f->linenum, f->filename);
    return (0);
  }

  return (1);
}
