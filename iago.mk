TARGET_NO_KERNEL := false
TARGET_NO_BOOTLOADER := false

# Command line tools needed by Iago installer
PRODUCT_PACKAGES += \
	resize2fs \
	e2fsck \
	tune2fs \
	ntfsresize \
	iagod \
	efibootmgr \

