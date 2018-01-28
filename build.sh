#!/bin/bash

export ARCH="arm"
export KBUILD_BUILD_HOST="eOS-0.4.1-Loki"
export KBUILD_BUILD_USER="arttttt"

clean_build=0
config="tegra12_android_defconfig"
kernel_name="SmokeR24.2"
threads=5
toolchain="$HOME/Android/toolchain/linaro-4.9.4/bin/arm-linux-gnueabihf-"

KERNEL_DIR=$PWD
ORIGINAL_OUTPUT_DIR="$KERNEL_DIR/arch/$ARCH/boot"
OUTPUT_DIR="$KERNEL_DIR/output"

generate_version()
{
	if [[ -f "$KERNEL_DIR/.git/HEAD"  &&  -f "$KERNEL_DIR/anykernel/anykernel.sh" ]]; then
		local updated_kernel_name
		eval "$(awk -F"="  '/kernel.string/{print "anykernel_name="$2}' $KERNEL_DIR/anykernel/anykernel.sh)"
		eval "$(awk -F"-"  '{print "current_branch="$2}' $KERNEL_DIR/.git/HEAD)"
		if [[ "$current_branch" != "stable" ]]; then
			if [[ ! -f "$KERNEL_DIR/version" ]]; then
				echo "build_number=0" > $KERNEL_DIR/version
			fi;

			awk -F"="  '{$2+=1; print $1"="$2}' $KERNEL_DIR/version > tmpfile
			mv tmpfile $KERNEL_DIR/version
			eval "$(awk -F"="  '{print "current_build="$2}' $KERNEL_DIR/version)"
			export LOCALVERSION="-build$current_build"
			updated_kernel_name=$kernel_name"-"$current_branch"-build"$current_build
		else
			updated_kernel_name=$kernel_name"-"$current_branch
		fi;
			sed -i s/$anykernel_name/$updated_kernel_name/ $KERNEL_DIR/anykernel/anykernel.sh
	fi;
}

make_zip()
{
	printf "\nСоздание zip архива\n\n"

	cd $KERNEL_DIR/anykernel
	local zip_name="$kernel_name($(date +'%d.%m.%Y-%H:%M')).zip"
	zip -r $zip_name *

	if [[ -f "$PWD/$zip_name" ]]; then
		printf "\n$zip_name создан, перемещение в $OUTPUT_DIR"
		mv "$PWD/$zip_name" $OUTPUT_DIR

		if [[ -f "$OUTPUT_DIR/$zip_name" ]]; then
			echo
			printf "\nЗавершено\n"
		fi
	else
		printf "\nНе удалось создать архив\n"
		exit 0
	fi
	cd $KERNEL_DIR
}

compile()
{
	local start=$(date +%s)
	clear

	if [[ "$clean_build" == 1 ]]; then
		make clean
		make mrproper
	fi

	generate_version
	make $config
	make -j$threads ARCH=$ARCH CROSS_COMPILE=$toolchain zImage

	if [[ -f "$ORIGINAL_OUTPUT_DIR/zImage" ]]; then
		mv $ORIGINAL_OUTPUT_DIR/zImage $PWD/anykernel/kernel/
	else
		exit 0
	fi

	printf "\nКомпиляция дерева устройства\n\n"

	make -j$threads ARCH=$ARCH CROSS_COMPILE=$toolchain tegra124-mocha.dtb

	if [[ -f "$ORIGINAL_OUTPUT_DIR/dts/tegra124-mocha.dtb" ]]; then
		mv $ORIGINAL_OUTPUT_DIR/dts/tegra124-mocha.dtb $PWD/anykernel/kernel/dtb
	else
		exit 0
	fi

	local end=$(date +%s)
	local comp_time=$((end-start))
	printf "\nЯдро скомпилировано за %02d:%02d\n" $((($comp_time/60)%60)) $(($comp_time%60))
	printf "Сборка номер %d в ветке %s" $current_build $current_branch

	make_zip
}

compile_dtb()
{
	clear

	generate_version
	make $config
	make -j$threads ARCH=$ARCH CROSS_COMPILE=$toolchain tegra124-mocha.dtb

	if [[ -f "$ORIGINAL_OUTPUT_DIR/dts/tegra124-mocha.dtb" ]]; then
		mv $ORIGINAL_OUTPUT_DIR/dts/tegra124-mocha.dtb $PWD/anykernel/kernel/dtb
	else
		exit 0
	fi

	make_zip
}

main()
{
	clear
	echo "---------------------------------------------------"
	echo "Выполнить чистую сборку?                          -"
	echo "---------------------------------------------------"
	echo "1 - Да                                            -"
	echo "---------------------------------------------------"
	echo "2 - Нет                                           -"
	echo "---------------------------------------------------"
	echo "3 - Собрать dtb                                   -"
	echo "---------------------------------------------------"
	echo "4 - Выход                                         -"
	echo "---------------------------------------------------"
	printf %s "Ваш выбор: "
	read env

	case $env in
		1) clean_build=1;compile;;
		2) compile;;
		3) compile_dtb;;
		4) exit 0;;
		*) main;;
	esac
}

main
