/*
 *   (Tentative) USB Audio Driver for ALSA
 *
 *   Copyright (c) 2002 by Takashi Iwai <tiwai@suse.de>
 *
 *   Many codes borrowed from audio.c by
 *	    Alan Cox (alan@lxorguk.ukuu.org.uk)
 *	    Thomas Sailer (sailer@ife.ee.ethz.ch)
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 *
 *  NOTES:
 *
 *   - the linked URBs would be preferred but not used so far because of
 *     the instability of unlinking.
 *   - type II is not supported properly.  there is no device which supports
 *     this type *correctly*.  SB extigy looks as if it supports, but it's
 *     indeed an AC3 stream packed in SPDIF frames (i.e. no real AC3 stream).
 */


#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/usb.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/usb/audio.h>
#include <linux/usb/audio-v2.h>
#include <linux/module.h>

#include <sound/control.h>
#include <sound/core.h>
#include <sound/info.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

#include "usbaudio.h"
#include "card.h"
#include "midi.h"
#include "mixer.h"
#include "proc.h"
#include "quirks.h"
#include "endpoint.h"
#include "helper.h"
#include "debug.h"
#include "pcm.h"
#include "format.h"
#include "power.h"
#include "stream.h"

MODULE_AUTHOR("Takashi Iwai <tiwai@suse.de>");
MODULE_DESCRIPTION("USB Audio");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{Generic,USB Audio}}");


static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;/* Enable this card */
/* Vendor/product IDs for this card */
static int vid[SNDRV_CARDS] = { [0 ... (SNDRV_CARDS-1)] = -1 };
static int pid[SNDRV_CARDS] = { [0 ... (SNDRV_CARDS-1)] = -1 };
static int nrpacks = 8;		/* max. number of packets per urb */
static int device_setup[SNDRV_CARDS]; /* device parameter for this card */
static bool ignore_ctl_error;
static bool autoclock = true;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the USB audio adapter.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the USB audio adapter.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable USB audio adapter.");
module_param_array(vid, int, NULL, 0444);
MODULE_PARM_DESC(vid, "Vendor ID for the USB audio device.");
module_param_array(pid, int, NULL, 0444);
MODULE_PARM_DESC(pid, "Product ID for the USB audio device.");
module_param(nrpacks, int, 0644);
MODULE_PARM_DESC(nrpacks, "Max. number of packets per URB.");
module_param_array(device_setup, int, NULL, 0444);
MODULE_PARM_DESC(device_setup, "Specific device setup (if needed).");
module_param(ignore_ctl_error, bool, 0444);
MODULE_PARM_DESC(ignore_ctl_error,
		 "Ignore errors from USB controller for mixer interfaces.");
module_param(autoclock, bool, 0444);
MODULE_PARM_DESC(autoclock, "Enable auto-clock selection for UAC2 devices (default: yes).");

/*
 * we keep the snd_usb_audio_t instances by ourselves for merging
 * the all interfaces on the same card as one sound device.
 */

static DEFINE_MUTEX(register_mutex);
static struct snd_usb_audio *usb_chip[SNDRV_CARDS];
static struct usb_driver usb_audio_driver;

/*
 * disconnect streams
 * called from snd_usb_audio_disconnect()
 */
static void snd_usb_stream_disconnect(struct list_head *head)
{
	int idx;
	struct snd_usb_stream *as;
	struct snd_usb_substream *subs;

	as = list_entry(head, struct snd_usb_stream, list);
	for (idx = 0; idx < 2; idx++) {
		subs = &as->substream[idx];
		if (!subs->num_formats)
			continue;
		subs->interface = -1;
		subs->data_endpoint = NULL;
		subs->sync_endpoint = NULL;
	}
}

static int snd_usb_create_stream(struct snd_usb_audio *chip, int ctrlif, int interface)
{
	struct usb_device *dev = chip->dev;
	struct usb_host_interface *alts;
	struct usb_interface_descriptor *altsd;
	struct usb_interface *iface = usb_ifnum_to_if(dev, interface);

	if (!iface) {
		snd_printk(KERN_ERR "%d:%u:%d : does not exist\n",
			   dev->devnum, ctrlif, interface);
		return -EINVAL;
	}

	alts = &iface->altsetting[0];
	altsd = get_iface_desc(alts);

	/*
	 * Android with both accessory and audio interfaces enabled gets the
	 * interface numbers wrong.
	 */
	if ((chip->usb_id == USB_ID(0x18d1, 0x2d04) ||
	     chip->usb_id == USB_ID(0x18d1, 0x2d05)) &&
	    interface == 0 &&
	    altsd->bInterfaceClass == USB_CLASS_VENDOR_SPEC &&
	    altsd->bInterfaceSubClass == USB_SUBCLASS_VENDOR_SPEC) {
		interface = 2;
		iface = usb_ifnum_to_if(dev, interface);
		if (!iface)
			return -EINVAL;
		alts = &iface->altsetting[0];
		altsd = get_iface_desc(alts);
	}

	if (usb_interface_claimed(iface)) {
		snd_printdd(KERN_INFO "%d:%d:%d: skipping, already claimed\n",
						dev->devnum, ctrlif, interface);
		return -EINVAL;
	}

	if ((altsd->bInterfaceClass == USB_CLASS_AUDIO ||
	     altsd->bInterfaceClass == USB_CLASS_VENDOR_SPEC) &&
	    altsd->bInterfaceSubClass == USB_SUBCLASS_MIDISTREAMING) {
		int err = snd_usbmidi_create(chip->card, iface,
					     &chip->midi_list, NULL);
		if (err < 0) {
			snd_printk(KERN_ERR "%d:%u:%d: cannot create sequencer device\n",
						dev->devnum, ctrlif, interface);
			return -EINVAL;
		}
		usb_driver_claim_interface(&usb_audio_driver, iface, (void *)-1L);

		return 0;
	}

	if ((altsd->bInterfaceClass != USB_CLASS_AUDIO &&
	     altsd->bInterfaceClass != USB_CLASS_VENDOR_SPEC) ||
	    altsd->bInterfaceSubClass != USB_SUBCLASS_AUDIOSTREAMING) {
		snd_printdd(KERN_ERR "%d:%u:%d: skipping non-supported interface %d\n",
					dev->devnum, ctrlif, interface, altsd->bInterfaceClass);
		/* skip non-supported classes */
		return -EINVAL;
	}

	if (snd_usb_get_speed(dev) == USB_SPEED_LOW) {
		snd_printk(KERN_ERR "low speed audio streaming not supported\n");
		return -EINVAL;
	}

	if (! snd_usb_parse_audio_interface(chip, interface)) {
		usb_set_interface(dev, interface, 0); /* reset the current interface */
		usb_driver_claim_interface(&usb_audio_driver, iface, (void *)-1L);
	}

	return 0;
}

/*
 * parse audio control descriptor and create pcm/midi streams
 */
static int snd_usb_create_streams(struct snd_usb_audio *chip, int ctrlif)
{
	struct usb_device *dev = chip->dev;
	struct usb_host_interface *host_iface;
	struct usb_interface_descriptor *altsd;
	void *control_header;
	int i, protocol;

	/* find audiocontrol interface */
	host_iface = &usb_ifnum_to_if(dev, ctrlif)->altsetting[0];
	control_header = snd_usb_find_csint_desc(host_iface->extra,
						 host_iface->extralen,
						 NULL, UAC_HEADER);
	altsd = get_iface_desc(host_iface);
	protocol = altsd->bInterfaceProtocol;

	if (!control_header) {
		snd_printk(KERN_ERR "cannot find UAC_HEADER\n");
		return -EINVAL;
	}

	switch (protocol) {
	default:
		snd_printdd(KERN_WARNING "unknown interface protocol %#02x, assuming v1\n",
			    protocol);
		/* fall through */

	case UAC_VERSION_1: {
		struct uac1_ac_header_descriptor *h1 = control_header;

		if (!h1->bInCollection) {
			snd_printk(KERN_INFO "skipping empty audio interface (v1)\n");
			return -EINVAL;
		}

		if (h1->bLength < sizeof(*h1) + h1->bInCollection) {
			snd_printk(KERN_ERR "invalid UAC_HEADER (v1)\n");
			return -EINVAL;
		}

		for (i = 0; i < h1->bInCollection; i++)
			snd_usb_create_stream(chip, ctrlif, h1->baInterfaceNr[i]);

		break;
	}

	case UAC_VERSION_2: {
		struct usb_interface_assoc_descriptor *assoc =
			usb_ifnum_to_if(dev, ctrlif)->intf_assoc;

		if (!assoc) {
			/*
			 * Firmware writers cannot count to three.  So to find
			 * the IAD on the NuForce UDH-100, also check the next
			 * interface.
			 */
			struct usb_interface *iface =
				usb_ifnum_to_if(dev, ctrlif + 1);
			if (iface &&
			    iface->intf_assoc &&
			    iface->intf_assoc->bFunctionClass == USB_CLASS_AUDIO &&
			    iface->intf_assoc->bFunctionProtocol == UAC_VERSION_2)
				assoc = iface->intf_assoc;
		}

		if (!assoc) {
			snd_printk(KERN_ERR "Audio class v2 interfaces need an interface association\n");
			return -EINVAL;
		}

		for (i = 0; i < assoc->bInterfaceCount; i++) {
			int intf = assoc->bFirstInterface + i;

			if (intf != ctrlif)
				snd_usb_create_stream(chip, ctrlif, intf);
		}

		break;
	}
	}

	return 0;
}

/*
 * free the chip instance
 *
 * here we have to do not much, since pcm and controls are already freed
 *
 */

static int snd_usb_audio_free(struct snd_usb_audio *chip)
{
	mutex_destroy(&chip->mutex);
	kfree(chip);
	return 0;
}

static int snd_usb_audio_dev_free(struct snd_device *device)
{
	struct snd_usb_audio *chip = device->device_data;
	return snd_usb_audio_free(chip);
}

static void remove_trailing_spaces(char *str)
{
	char *p;

	if (!*str)
		return;
	for (p = str + strlen(str) - 1; p >= str && isspace(*p); p--)
		*p = 0;
}
/*proc info of usb sound card*/
struct usb_audio_source_config{
    int card;
    int device;
    u32 usbid;
    struct mutex usb_audio_mutex;
};

static struct snd_info_entry *snd_usb_audio_sourece_info_entry;
static int usb_audio_info_exit = 0;
static struct usb_audio_source_config * pstr,usbaudioinfo[9]; //usb_card_NUm=1 ~ 8
//static struct usb_audio_source_config  snd_usb_config;
// insert usb audio info to usb_audio_source_config which from user
static int usb_audio_source_info_Insert(u32 usbid, int num)
{
    int i = 0, j=0;
    struct mutex usb_audio_mutex2;
    struct usb_audio_source_config u1;

    memset(&u1, 0, sizeof(struct usb_audio_source_config));
    mutex_init(&usb_audio_mutex2);
    mutex_lock(&usb_audio_mutex2);
    for(i = 1; i <= num; i++)
    {
        if(usbaudioinfo[i].usbid == usbid)
        {
            u1.usbid = usbid;
            u1.card = usbaudioinfo[i].card;
            u1.device = usbaudioinfo[i].device;
            for(j = i; j < num; j++)
            {
                usbaudioinfo[j].card = usbaudioinfo[j+1].card;
                usbaudioinfo[j].device = usbaudioinfo[j+1].device;
                usbaudioinfo[j].usbid = usbaudioinfo[j+1].usbid;
            }
        }
    }
    usbaudioinfo[num].usbid = u1.usbid;
    usbaudioinfo[num].card = u1.card;
    usbaudioinfo[num].device = u1.device;

    pstr->card = u1.card;
    pstr->device = u1.device;
    pstr->usbid = u1.usbid;
    mutex_unlock(&usb_audio_mutex2);
    return 0;
}

// make the lastest usb audio as default usb audio
static int usb_audio_source_info_order(u32 usbid, int num)
{
    int i = 0, j=0;
    struct mutex usb_audio_mutex1;
    mutex_init(&usb_audio_mutex1);
    mutex_lock(&usb_audio_mutex1);
    for(i = 1; i <= num; i++)
    {
        if(usbaudioinfo[i].usbid == usbid)
        {
            for(j=i; j < num; j++)
            {
                usbaudioinfo[j].card = usbaudioinfo[j+1].card;
                usbaudioinfo[j].device = usbaudioinfo[j+1].device;
                usbaudioinfo[j].usbid = usbaudioinfo[j+1].usbid;
            }
        }
    }
    pstr->card = usbaudioinfo[num-1].card;
    pstr->device = usbaudioinfo[num-1].device;
    pstr->usbid = usbaudioinfo[num-1].usbid;
    mutex_unlock(&usb_audio_mutex1);
    return 0;
}
static void usb_audio_source_info_read(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
    printk("USB sound usbid = 0x%08x, card = %d, device = %d**\n", pstr->usbid, pstr->card, pstr->device);
    snd_iprintf(buffer, "%08x %d %d\n", pstr->usbid, pstr->card, pstr->device);
}

static void usb_audio_source_info_write(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
    char line[128], str[32];
    const char *cptr;
    int pos=0,i=0,idx;

   // printk("***buffer->curr=%d,buffer->size=%d,buffer->len=%d,buffer->stop=%d,buffer->error=%d\n",
   //     buffer->curr,buffer->size,buffer->len,buffer->stop,buffer->error);

    while (!snd_info_get_line(buffer,line,sizeof(line))){
        mutex_lock(&pstr->usb_audio_mutex);
        cptr = snd_info_get_str(str, line, sizeof(str));

    //    printk("***snd_info_get_str**str=%s, cptr=%s**\n",str,cptr);

        //judge usb audio usbid whether true

        idx = simple_strtoul(str, NULL, 16);
        for(i=1; i<=usb_audio_info_exit; i++)
        {
            if(idx == usbaudioinfo[i].usbid)
            {
                pos = i;
                break;
            }
        }
     //   printk("****i=%d**idx=%d***pos=%d**\n",i,idx,pos);
        if(i > usb_audio_info_exit)
        {
            printk("****have no this usb card !****\n");
            mutex_unlock(&pstr->usb_audio_mutex);
            return;
        }
        //judge usb audio card whether true
        cptr = snd_info_get_str(str, cptr, sizeof(str));
        idx = simple_strtoul(str,NULL,10);
        if(idx != usbaudioinfo[pos].card)
        {
            printk("****card number wrong !*****\n");
            mutex_unlock(&pstr->usb_audio_mutex);
            return;
        }
        //judge usb audio device whether true
        idx = simple_strtoul(cptr,NULL,10);
        if(cptr)
        {
            if(idx != usbaudioinfo[pos].device)
            {
                printk("****device number wrong !*****\n");
                mutex_unlock(&pstr->usb_audio_mutex);
                return;
            }
        }

        usb_audio_source_info_Insert(usbaudioinfo[pos].usbid, usb_audio_info_exit);
        mutex_unlock(&pstr->usb_audio_mutex);

    }


}

/* list to retrieve the usb id and card number */
static LIST_HEAD(usb_entry_list);
static DEFINE_MUTEX(audio_mutex);

struct usb_id_info{
	struct snd_info_entry *entry;
	int card_num;
	int usb_id;
	struct list_head list;
};

static void usb_audio_id_card_info_read(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
	struct usb_id_info *id_info = (struct usb_id_info *)entry->private_data;

	snd_iprintf(buffer, "%d\n", id_info->card_num);
}
#define id_size 32
static int usb_audio_id_card_init(int id, int card)
{
	struct snd_info_entry *entry = NULL;
	struct usb_id_info *id_info = NULL;
	char id_string[id_size];
	int err = 0;

	id_info = kzalloc(sizeof(struct usb_id_info), GFP_KERNEL);
	if (id_info == NULL){
		err = -ENOMEM;
		goto err1;
	}

	snprintf(id_string, 13, "USB-%08x", id);
	snd_printk("usb_audio_id_card_init, id_string== %s\n", id_string);
	if((entry = snd_info_create_module_entry(THIS_MODULE, id_string, NULL)) == NULL){
		err = -ENOMEM;
		goto err2;
	}

	entry->private_data = id_info;
	entry->content = SNDRV_INFO_CONTENT_TEXT;
	entry->mode = S_IFREG | S_IRUGO;
	entry->c.text.read = usb_audio_id_card_info_read;
	if (snd_info_register(entry) < 0) {
		err = -ENOMEM;
		goto err3;
	}

	id_info->entry = entry;
	id_info->usb_id = id;
	id_info->card_num = card;
	mutex_lock(&audio_mutex);
	list_add(&id_info->list, &usb_entry_list);
	mutex_unlock(&audio_mutex);

	return 0;

err3:
	snd_info_free_entry(entry);
err2:
	kfree(id_info);
err1:
	return err;
}

static int usb_audio_id_card_deinit(int id)
{
	struct usb_id_info *id_info;
	struct snd_info_entry *entry;

	mutex_lock(&audio_mutex);
	list_for_each_entry(id_info, &usb_entry_list, list) {
		snd_printk(KERN_DEBUG "list_for_each_entry at usb_audio_id_card_deinit, usb_id = %x, id =%x\n", id_info->usb_id,id);
		if (id_info->usb_id == id){
			entry = id_info->entry;
			goto found;
		}
	}
	mutex_unlock(&audio_mutex);

	return -EINVAL;

found:
	list_del(&id_info->list);
	mutex_unlock(&audio_mutex);
	snd_info_free_entry(entry);
	kfree(id_info);
	id_info = NULL;
	return 0;
}

static int usb_audio_source_info_init(struct usb_audio_source_config * pstr)
{

    struct snd_info_entry *entry;
    usb_audio_info_exit += 1;
    if(usb_audio_info_exit >=8)
    {
        printk("***add too many usb audio card***\n");
        return -ENOMEM;
    }

    usbaudioinfo[usb_audio_info_exit].card = pstr->card;
    usbaudioinfo[usb_audio_info_exit].device = pstr->device;
    usbaudioinfo[usb_audio_info_exit].usbid = pstr->usbid;
    if(usb_audio_info_exit ==1)
    {
        entry = snd_info_create_module_entry(THIS_MODULE, "usb_audio_info", NULL);

        if (entry == NULL)
            return -ENOMEM;
        entry->content = SNDRV_INFO_CONTENT_TEXT;
        entry->mode = S_IFREG | S_IRUGO | S_IWUSR;
        entry->c.text.read = usb_audio_source_info_read;
        entry->c.text.write = usb_audio_source_info_write;
        //entry->private_data = pstr;
        if (snd_info_register(entry) < 0) {
            snd_info_free_entry(entry);
            return -ENOMEM;
        }
        snd_usb_audio_sourece_info_entry = entry;
    }

    return 0;
}

static int usb_audio_source_info_done(u32 usbid)
{
    usb_audio_info_exit -= 1;

    if(!usb_audio_info_exit)
        snd_info_free_entry(snd_usb_audio_sourece_info_entry);
    else
        usb_audio_source_info_order(usbid, usb_audio_info_exit + 1);

    return 0;
}

/*
 * create a chip instance and set its names.
 */
static int snd_usb_audio_create(struct usb_device *dev, int idx,
				const struct snd_usb_audio_quirk *quirk,
				struct snd_usb_audio **rchip)
{
	struct snd_card *card;
	struct snd_usb_audio *chip;
	int err, len;
	char component[14];
	static struct snd_device_ops ops = {
		.dev_free =	snd_usb_audio_dev_free,
	};

	*rchip = NULL;

	switch (snd_usb_get_speed(dev)) {
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
	case USB_SPEED_SUPER:
		break;
	default:
		snd_printk(KERN_ERR "unknown device speed %d\n", snd_usb_get_speed(dev));
		return -ENXIO;
	}

	err = snd_card_create(index[idx], id[idx], THIS_MODULE, 0, &card);
	if (err < 0) {
		snd_printk(KERN_ERR "cannot create card instance %d\n", idx);
		return err;
	}

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (! chip) {
		snd_card_free(card);
		return -ENOMEM;
	}

	mutex_init(&chip->mutex);
	init_rwsem(&chip->shutdown_rwsem);
	chip->index = idx;
	chip->dev = dev;
	chip->card = card;
	chip->setup = device_setup[idx];
	chip->nrpacks = nrpacks;
	chip->autoclock = autoclock;
	chip->probing = 1;

	chip->usb_id = USB_ID(le16_to_cpu(dev->descriptor.idVendor),
			      le16_to_cpu(dev->descriptor.idProduct));

	err = usb_audio_id_card_init(chip->usb_id, card->number);
	if (err < 0) {
		snd_printk(KERN_ERR "cannot create id card instance usb_id=(0x%08x)\n", chip->usb_id);
		return err;
	}

	INIT_LIST_HEAD(&chip->pcm_list);
	INIT_LIST_HEAD(&chip->ep_list);
	INIT_LIST_HEAD(&chip->midi_list);
	INIT_LIST_HEAD(&chip->mixer_list);
    pstr = kzalloc(sizeof(*pstr), GFP_KERNEL);
    if (! pstr) {
        kzfree(pstr);
        return -ENOMEM;
    }
    mutex_init(&pstr->usb_audio_mutex);
    pstr->card = card->number;
    pstr->device = 0;
    pstr->usbid = chip->usb_id;

   // add "usb_audio_info" node in path: /proc/asound/
    usb_audio_source_info_init(pstr);
	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops)) < 0) {
		snd_usb_audio_free(chip);
		snd_card_free(card);
		return err;
	}

	strcpy(card->driver, "USB-Audio");
	sprintf(component, "USB%04x:%04x",
		USB_ID_VENDOR(chip->usb_id), USB_ID_PRODUCT(chip->usb_id));
	snd_component_add(card, component);

	/* retrieve the device string as shortname */
	if (quirk && quirk->product_name && *quirk->product_name) {
		strlcpy(card->shortname, quirk->product_name, sizeof(card->shortname));
	} else {
		if (!dev->descriptor.iProduct ||
		    usb_string(dev, dev->descriptor.iProduct,
		    card->shortname, sizeof(card->shortname)) <= 0) {
			/* no name available from anywhere, so use ID */
			sprintf(card->shortname, "USB Device %#04x:%#04x",
				USB_ID_VENDOR(chip->usb_id),
				USB_ID_PRODUCT(chip->usb_id));
		}
	}
	remove_trailing_spaces(card->shortname);

	/* retrieve the vendor and device strings as longname */
	if (quirk && quirk->vendor_name && *quirk->vendor_name) {
		len = strlcpy(card->longname, quirk->vendor_name, sizeof(card->longname));
	} else {
		if (dev->descriptor.iManufacturer)
			len = usb_string(dev, dev->descriptor.iManufacturer,
					 card->longname, sizeof(card->longname));
		else
			len = 0;
		/* we don't really care if there isn't any vendor string */
	}
	if (len > 0) {
		remove_trailing_spaces(card->longname);
		if (*card->longname)
			strlcat(card->longname, " ", sizeof(card->longname));
	}

	strlcat(card->longname, card->shortname, sizeof(card->longname));

	len = strlcat(card->longname, " at ", sizeof(card->longname));

	if (len < sizeof(card->longname))
		usb_make_path(dev, card->longname + len, sizeof(card->longname) - len);

	switch (snd_usb_get_speed(dev)) {
	case USB_SPEED_LOW:
		strlcat(card->longname, ", low speed", sizeof(card->longname));
		break;
	case USB_SPEED_FULL:
		strlcat(card->longname, ", full speed", sizeof(card->longname));
		break;
	case USB_SPEED_HIGH:
		strlcat(card->longname, ", high speed", sizeof(card->longname));
		break;
	case USB_SPEED_SUPER:
		strlcat(card->longname, ", super speed", sizeof(card->longname));
		break;
	default:
		break;
	}

	snd_usb_audio_create_proc(chip);

	*rchip = chip;
	return 0;
}

/*
 * probe the active usb device
 *
 * note that this can be called multiple times per a device, when it
 * includes multiple audio control interfaces.
 *
 * thus we check the usb device pointer and creates the card instance
 * only at the first time.  the successive calls of this function will
 * append the pcm interface to the corresponding card.
 */
static struct snd_usb_audio *
snd_usb_audio_probe(struct usb_device *dev,
		    struct usb_interface *intf,
		    const struct usb_device_id *usb_id)
{
	const struct snd_usb_audio_quirk *quirk = (const struct snd_usb_audio_quirk *)usb_id->driver_info;
	int i, err, send_env = 0;
	struct snd_usb_audio *chip;
	struct usb_host_interface *alts;
	int ifnum;
	u32 id;
	char event_state[32], event_card[32], event_id[32];
	char *envp[] = {event_state, event_card, event_id, NULL};

	alts = &intf->altsetting[0];
	ifnum = get_iface_desc(alts)->bInterfaceNumber;
	id = USB_ID(le16_to_cpu(dev->descriptor.idVendor),
		    le16_to_cpu(dev->descriptor.idProduct));
	if (quirk && quirk->ifnum >= 0 && ifnum != quirk->ifnum)
		goto __err_val;

	if (snd_usb_apply_boot_quirk(dev, intf, quirk) < 0)
		goto __err_val;

	/*
	 * found a config.  now register to ALSA
	 */

	/* check whether it's already registered */
	chip = NULL;
	mutex_lock(&register_mutex);
	for (i = 0; i < SNDRV_CARDS; i++) {
		if (usb_chip[i] && usb_chip[i]->dev == dev) {
			if (usb_chip[i]->shutdown) {
				snd_printk(KERN_ERR "USB device is in the shutdown state, cannot create a card instance\n");
				goto __error;
			}
			chip = usb_chip[i];
			chip->probing = 1;
			break;
		}
	}
	if (! chip) {
		/* it's a fresh one.
		 * now look for an empty slot and create a new card instance
		 */
		for (i = 0; i < SNDRV_CARDS; i++)
			if (enable[i] && ! usb_chip[i] &&
			    (vid[i] == -1 || vid[i] == USB_ID_VENDOR(id)) &&
			    (pid[i] == -1 || pid[i] == USB_ID_PRODUCT(id))) {
				if (snd_usb_audio_create(dev, i, quirk, &chip) < 0) {
					goto __error;
				}
				snd_card_set_dev(chip->card, &intf->dev);
				chip->pm_intf = intf;
				break;
			}
		if (!chip) {
			printk(KERN_ERR "no available usb audio device\n");
			goto __error;
		}
		send_env = 1;
	}

	/*
	 * For devices with more than one control interface, we assume the
	 * first contains the audio controls. We might need a more specific
	 * check here in the future.
	 */
	if (!chip->ctrl_intf)
		chip->ctrl_intf = alts;

	chip->txfr_quirk = 0;
	err = 1; /* continue */
	if (quirk && quirk->ifnum != QUIRK_NO_INTERFACE) {
		/* need some special handlings */
		if ((err = snd_usb_create_quirk(chip, intf, &usb_audio_driver, quirk)) < 0)
			goto __error;
	}

	if (err > 0) {
		/* create normal USB audio interfaces */
		if (snd_usb_create_streams(chip, ifnum) < 0 ||
		    snd_usb_create_mixer(chip, ifnum, ignore_ctl_error) < 0) {
			goto __error;
		}
	}

	/* we are allowed to call snd_card_register() many times */
	if (snd_card_register(chip->card) < 0) {
		goto __error;
	}
	if (1 == send_env) {
		snprintf(event_state, 32, "USB_AUDIO_DEVICE=ADD");
		snprintf(event_card, 32, "USB_AUDIO_CARD=%01d", chip->card->number);
		snprintf(event_id, 32, "USB_AUDIO_ID=%08x", chip->usb_id);
		kobject_uevent_env(&(dev->dev.kobj), KOBJ_ADD, envp);
	}
	usb_chip[chip->index] = chip;
	chip->num_interfaces++;
	chip->probing = 0;
	mutex_unlock(&register_mutex);
	return chip;

 __error:
	if (chip) {
		if (!chip->num_interfaces)
			snd_card_free(chip->card);
		chip->probing = 0;
	}
	mutex_unlock(&register_mutex);
 __err_val:
	return NULL;
}

/*
 * we need to take care of counter, since disconnection can be called also
 * many times as well as usb_audio_probe().
 */
static void snd_usb_audio_disconnect(struct usb_device *dev,
				     struct snd_usb_audio *chip)
{
	struct snd_card *card;
	struct list_head *p, *n;
	char event_state[32], event_card[32], event_id[32];
	char *envp[] = {event_state, event_card, event_id, NULL};
	if (chip == (void *)-1L)
		return;

	card = chip->card;
	down_write(&chip->shutdown_rwsem);
	chip->shutdown = 1;
	up_write(&chip->shutdown_rwsem);

	mutex_lock(&register_mutex);
	chip->num_interfaces--;
	if (chip->num_interfaces <= 0) {
		snd_card_disconnect(card);
		/* release the pcm resources */
		list_for_each(p, &chip->pcm_list) {
			snd_usb_stream_disconnect(p);
		}
		/* release the endpoint resources */
		list_for_each_safe(p, n, &chip->ep_list) {
			snd_usb_endpoint_free(p);
		}
		/* release the midi resources */
		list_for_each(p, &chip->midi_list) {
			snd_usbmidi_disconnect(p);
		}
		/* release mixer resources */
		list_for_each(p, &chip->mixer_list) {
			snd_usb_mixer_disconnect(p);
		}
		usb_chip[chip->index] = NULL;
		mutex_unlock(&register_mutex);
		snprintf(event_state, 32, "USB_AUDIO_DEVICE=REMOVE");
		snprintf(event_card, 32, "USB_AUDIO_CARD=%01d", chip->card->number);
		snprintf(event_id, 32, "USB_AUDIO_ID=%08x", chip->usb_id);
		kobject_uevent_env(&(dev->dev.kobj), KOBJ_REMOVE, envp);
		snd_card_free_when_closed(card);
		if(usb_audio_id_card_deinit(chip->usb_id) < 0)
			snd_printk(KERN_ERR "failed to deinit the usb audio card id(0x%08x)\n", chip->usb_id);;
	} else {
		mutex_unlock(&register_mutex);
	}
	usb_audio_source_info_done(chip->usb_id);
}

/*
 * new 2.5 USB kernel API
 */
static int usb_audio_probe(struct usb_interface *intf,
			   const struct usb_device_id *id)
{
	struct snd_usb_audio *chip;
	chip = snd_usb_audio_probe(interface_to_usbdev(intf), intf, id);
	if (chip) {
		usb_set_intfdata(intf, chip);
		return 0;
	} else
		return -EIO;
}

static void usb_audio_disconnect(struct usb_interface *intf)
{
	snd_usb_audio_disconnect(interface_to_usbdev(intf),
				 usb_get_intfdata(intf));
}

#ifdef CONFIG_PM

int snd_usb_autoresume(struct snd_usb_audio *chip)
{
	int err = -ENODEV;

	down_read(&chip->shutdown_rwsem);
	if (chip->probing)
		err = 0;
	else if (!chip->shutdown)
		err = usb_autopm_get_interface(chip->pm_intf);
	up_read(&chip->shutdown_rwsem);

	return err;
}

void snd_usb_autosuspend(struct snd_usb_audio *chip)
{
	down_read(&chip->shutdown_rwsem);
	if (!chip->shutdown && !chip->probing)
		usb_autopm_put_interface(chip->pm_intf);
	up_read(&chip->shutdown_rwsem);
}

static int usb_audio_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct snd_usb_audio *chip = usb_get_intfdata(intf);
	struct snd_usb_stream *as;
	struct usb_mixer_interface *mixer;

	if (chip == (void *)-1L)
		return 0;

	if (!PMSG_IS_AUTO(message)) {
		snd_power_change_state(chip->card, SNDRV_CTL_POWER_D3hot);
		if (!chip->num_suspended_intf++) {
			list_for_each_entry(as, &chip->pcm_list, list) {
				snd_pcm_suspend_all(as->pcm);
				as->substream[0].need_setup_ep =
					as->substream[1].need_setup_ep = true;
			}
		}
	} else {
		/*
		 * otherwise we keep the rest of the system in the dark
		 * to keep this transparent
		 */
		if (!chip->num_suspended_intf++)
			chip->autosuspended = 1;
	}

	list_for_each_entry(mixer, &chip->mixer_list, list)
		snd_usb_mixer_inactivate(mixer);

	return 0;
}

static int usb_audio_resume(struct usb_interface *intf)
{
	struct snd_usb_audio *chip = usb_get_intfdata(intf);
	struct usb_mixer_interface *mixer;
	int err = 0;

	if (chip == (void *)-1L)
		return 0;
	if (--chip->num_suspended_intf)
		return 0;
	/*
	 * ALSA leaves material resumption to user space
	 * we just notify and restart the mixers
	 */
	list_for_each_entry(mixer, &chip->mixer_list, list) {
		err = snd_usb_mixer_activate(mixer);
		if (err < 0)
			goto err_out;
	}

	if (!chip->autosuspended)
		snd_power_change_state(chip->card, SNDRV_CTL_POWER_D0);
	chip->autosuspended = 0;

err_out:
	return err;
}
static int usb_audio_reset_resume(struct usb_interface *intf)
{
   return usb_audio_resume(intf);
}
#else
#define usb_audio_suspend	NULL
#define usb_audio_resume	NULL
#endif		/* CONFIG_PM */

static struct usb_device_id usb_audio_ids [] = {
#include "quirks-table.h"
    { .match_flags = (USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS),
      .bInterfaceClass = USB_CLASS_AUDIO,
      .bInterfaceSubClass = USB_SUBCLASS_AUDIOCONTROL },
    { }						/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, usb_audio_ids);

/*
 * entry point for linux usb interface
 */

static struct usb_driver usb_audio_driver = {
	.name =		"snd-usb-audio",
	.probe =	usb_audio_probe,
	.disconnect =	usb_audio_disconnect,
	.suspend =	usb_audio_suspend,
	.resume =	usb_audio_resume,
	.reset_resume = usb_audio_reset_resume,
	.id_table =	usb_audio_ids,
	.supports_autosuspend = 1,
};

static int __init snd_usb_audio_init(void)
{
	if (nrpacks < 1 || nrpacks > MAX_PACKS) {
		printk(KERN_WARNING "invalid nrpacks value.\n");
		return -EINVAL;
	}
	return usb_register(&usb_audio_driver);
}

static void __exit snd_usb_audio_cleanup(void)
{
	usb_deregister(&usb_audio_driver);
}

module_init(snd_usb_audio_init);
module_exit(snd_usb_audio_cleanup);
