#!/bin/bash

export ARCH="arm"
export KBUILD_BUILD_HOST=$(lsb_release -d | awk -F":"  '{print $2}' | sed -e 's/^[ \t]*//' | sed -r 's/[ ]+/-/g')
export KBUILD_BUILD_USER="arttttt"

clean_build=0
config="tegra12_android_defconfig"
dtb_name="tegra124-mocha.dtb"
dtb_only=0
kernel_name=$(git rev-parse --abbrev-ref HEAD)
cpus_count=$(grep -c ^processor /proc/cpuinfo)

KERNEL_DIR=$PWD
ORIGINAL_OUTPUT_DIR="$KERNEL_DIR/arch/$ARCH/boot"
OUTPUT_DIR="$KERNEL_DIR/output"

ERROR=0
HEAD=1
WARNING=2

function printfc() {
	if [[ $2 == $ERROR ]]; then
		printf "\e[1;31m$1\e[0m"
		return
	fi;
	if [[ $2 == $HEAD ]]; then
		printf "\e[1;32m$1\e[0m"
		return
	fi;
	if [[ $2 == $WARNING ]]; then
		printf "\e[1;35m$1\e[0m"
		return
	fi;
}

function generate_version()
{
	if [[ -f "$KERNEL_DIR/.git/HEAD"  &&  -f "$KERNEL_DIR/anykernel/anykernel.sh" ]]; then
		local updated_kernel_name
		eval "$(awk -F"="  '/kernel.string/{print "anykernel_name="$2}' $KERNEL_DIR/anykernel/anykernel.sh)"
		eval "$(echo $kernel_name | awk -F"-"  '{print "current_branch="$2}')"
		if [[ ("$current_branch" == "stable" || "$current_branch" == "staging") ]]; then
			updated_kernel_name=$kernel_name
		else
			if [[ ! -f "$KERNEL_DIR/version" ]]; then
				echo "build_number=0" > $KERNEL_DIR/version
			fi;

			awk -F"="  '{$2+=1; print $1"="$2}' $KERNEL_DIR/version > tmpfile
			mv tmpfile $KERNEL_DIR/version
			eval "$(awk -F"="  '{print "current_build="$2}' $KERNEL_DIR/version)"
			export LOCALVERSION="-$current_branch-build$current_build"
			updated_kernel_name=$kernel_name"-build"$current_build
		fi;

		if [[ $CI == true ]]; then
			updated_kernel_name="SmokeR24.1"
		fi

		sed -i s/$anykernel_name/$updated_kernel_name/ $KERNEL_DIR/anykernel/anykernel.sh
	fi;
}

function make_zip()
{
	if [[ -d "$KERNEL_DIR/anykernel" ]]; then
		printfc "\nСоздание zip архива\n\n" $HEAD
	else
		printfc "\nПапка $KERNEL_DIR/anykernel не существует\n\n" $ERROR
		return
	fi;

	if [[ -f "$ORIGINAL_OUTPUT_DIR/zImage" ]]; then
		mv $ORIGINAL_OUTPUT_DIR/zImage $PWD/anykernel/kernel/
	else
		if [[ $dtb_only == 0 ]]; then
			printfc "Файл $ORIGINAL_OUTPUT_DIR/zImage не существует\n\n" $ERROR
			return
		fi
	fi

	if [[ -f "$ORIGINAL_OUTPUT_DIR/dts/$dtb_name" ]]; then
		mv $ORIGINAL_OUTPUT_DIR/dts/$dtb_name $PWD/anykernel/kernel/dtb
	else
		if [[ $dtb_only == 0 ]]; then
			printfc "Файл $ORIGINAL_OUTPUT_DIR/dts/$dtb_name не существует\n\n" $ERROR
			return
		fi
	fi

	cd $KERNEL_DIR/anykernel

	if [[ $CI == true ]]; then
		zip_name=$KERNEL_ZIP
	else
		zip_name="$kernel_name($(date +'%d.%m.%Y-%H.%M')).zip"
	fi

	zip -r $zip_name *

	if [[ -f "$PWD/$zip_name" ]]; then
		if [[ ! -d "$OUTPUT_DIR" ]]; then
			mkdir $OUTPUT_DIR
		fi;

		printfc "\n$zip_name создан, перемещение в $OUTPUT_DIR" $HEAD
		mv "$PWD/$zip_name" $OUTPUT_DIR

		if [[ -f "$OUTPUT_DIR/$zip_name" ]]; then
			echo
			printfc "\nЗавершено\n" $HEAD
		fi
	else
		printfc "\nНе удалось создать архив\n" $ERROR
		return
	fi
	cd $KERNEL_DIR
}

function compile()
{
	local start=$(date +%s)
	clear

	if [[ "$clean_build" == 1 ]]; then
		make clean
		make mrproper
	fi

	generate_version
	make $config
	make -j$cpus_count ARCH=$ARCH CROSS_COMPILE=$toolchain zImage

	printfc "\nКомпиляция дерева устройства\n\n" $HEAD

	make -j$cpus_count ARCH=$ARCH CROSS_COMPILE=$toolchain $dtb_name

	local end=$(date +%s)
	local comp_time=$((end-start))
	printf "\e[1;32m\nЯдро скомпилировано за %02d:%02d\n\e[0m" $((($comp_time/60)%60)) $(($comp_time%60))
	printfc "Сборка номер $current_build в ветке $current_branch\n" $HEAD

	make_zip
}

function compile_dtb()
{
	clear

	dtb_only=1
	generate_version
	make $config
	make -j$cpus_count ARCH=$ARCH CROSS_COMPILE=$toolchain $dtb_name

	if [[ -f "$ORIGINAL_OUTPUT_DIR/dts/$dtb_name" ]]; then
		mv $ORIGINAL_OUTPUT_DIR/dts/$dtb_name $PWD/anykernel/kernel/dtb
	else
		printfc "Файл $ORIGINAL_OUTPUT_DIR/dts/$dtb_name не существует\n\n" $ERROR
		return
	fi

	make_zip
}

function main()
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
		4) clear;return;;
		*) main;;
	esac
}

if [[ $CI == true ]]; then
	clean_build=1
	toolchain="arm-linux-gnueabihf-"
	compile
else
	toolchain="$HOME/PROJECTS/MIPAD/linaro-4.9.4/bin/arm-linux-gnueabihf-"
	main
fi
