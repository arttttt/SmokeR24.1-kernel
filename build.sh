#!/bin/bash

export ARCH="arm"
export KBUILD_BUILD_HOST="eOS-0.4.1-Loki"
export KBUILD_BUILD_USER="arttttt"

clean_build=0
config="tegra12_defconfig"
dtb_name="tegra124-mocha.dtb"
dtb_only=0
build_log="build.log"
threads=5
toolchain="$HOME/PROJECTS/MIPAD/linaro-4.9.4/bin/arm-linux-gnueabihf-"

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

function make_img()
{
	if [[ -d "$KERNEL_DIR/Initramfs" ]]; then
		printfc "\nСоздание boot.img\n\n" $HEAD
	else
		printfc "\nПапка $KERNEL_DIR/Initramfs не существует\n\n" $ERROR
		return
	fi;

	if [[ -f "$ORIGINAL_OUTPUT_DIR/zImage" ]]; then
		mv $ORIGINAL_OUTPUT_DIR/zImage $PWD/Initramfs/
	else
		if [[ $dtb_only == 0 ]]; then
			printfc "Файл $ORIGINAL_OUTPUT_DIR/zImage не существует\n\n" $ERROR
			return
		fi
	fi

	if [[ -f "$ORIGINAL_OUTPUT_DIR/dts/$dtb_name" ]]; then
		mv $ORIGINAL_OUTPUT_DIR/dts/$dtb_name $PWD/Initramfs/dtb
	else
		if [[ $dtb_only == 0 ]]; then
			printfc "Файл $ORIGINAL_OUTPUT_DIR/dts/$dtb_name не существует\n\n" $ERROR
			return
		fi
	fi

	cd $KERNEL_DIR/Initramfs

	./build.sh

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

	make $config
	make -j$threads ARCH=$ARCH CROSS_COMPILE=$toolchain zImage

	local i=0
	while read line; do 
		error=$(echo "$line" | awk '/warning:/{print}')
		if [[ "$error" != "" ]]; then
			if [[ $i == 0 ]]; then
				printfc "\n\nСписок предупреждений компилятора:\n\n" $HEAD
				i+=1
			fi;
			printfc "$error\n" $WARNING
			error=""
		fi;
	done < $KERNEL_DIR/$build_log

	local error_status=0

	local i=0
	while read line; do 
		error=$(echo "$line" | awk '/error:/{print}')
		if [[ "$error" != "" ]]; then
			if [[ $i == 0 ]]; then
				printfc "\n\nСписок ошибок компиляции:\n\n" $HEAD
				i+=1
			fi;
			printfc "$error\n" $ERROR
			error=""
			error_status=1
		fi;
	done < $KERNEL_DIR/$build_log

	if [[ -f "$build_log" ]]; then
		rm $build_log
	fi;

	if [[ "$error_status" == 1 ]]; then
		printfc "\n\nСборка ядра прервана\n" $ERROR
		return
	fi;

	printfc "\nКомпиляция дерева устройства\n\n" $HEAD

	make -j$threads ARCH=$ARCH CROSS_COMPILE=$toolchain $dtb_name

	local end=$(date +%s)
	local comp_time=$((end-start))
	printf "\e[1;32m\nЯдро скомпилировано за %02d:%02d\n\e[0m" $((($comp_time/60)%60)) $(($comp_time%60))

	make_img
}

function compile_dtb()
{
	clear

	dtb_only=1
	make $config
	make -j$threads ARCH=$ARCH CROSS_COMPILE=$toolchain $dtb_name

	if [[ -f "$ORIGINAL_OUTPUT_DIR/dts/$dtb_name" ]]; then
		mv $ORIGINAL_OUTPUT_DIR/dts/$dtb_name $PWD/anykernel/kernel/dtb
	else
		printfc "Файл $ORIGINAL_OUTPUT_DIR/dts/$dtb_name не существует\n\n" $ERROR
		return
	fi

	make_img
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

main
