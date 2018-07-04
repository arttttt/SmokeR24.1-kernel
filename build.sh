#!/bin/bash

export ARCH="arm"
export KBUILD_BUILD_HOST="eOS-0.4.1-Loki"
export KBUILD_BUILD_USER="arttttt"

clean_build=0
config="tegra12_defconfig"
dtb_name="tegra124-mocha.dtb"
dtb_only=0
kernel_name="SmokeR24.1"
build_log="build.log"
threads=5
toolchain="$HOME/Android/toolchain/linaro-4.9.4/bin/arm-linux-gnueabihf-"

KERNEL_DIR=$PWD
ORIGINAL_OUTPUT_DIR="$KERNEL_DIR/arch/$ARCH/boot"
OUTPUT_DIR="$KERNEL_DIR/output"

ERROR=0
HEAD=1
WARNING=2

printfc() {
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
	local zip_name="$kernel_name($(date +'%d.%m.%Y-%H:%M')).zip"
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
	printfc "Сборка номер $current_build в ветке $current_branch\n" $HEAD

	make_zip
}

compile_dtb()
{
	clear

	dtb_only=1
	generate_version
	make $config
	make -j$threads ARCH=$ARCH CROSS_COMPILE=$toolchain $dtb_name

	if [[ -f "$ORIGINAL_OUTPUT_DIR/dts/$dtb_name" ]]; then
		mv $ORIGINAL_OUTPUT_DIR/dts/$dtb_name $PWD/anykernel/kernel/dtb
	else
		printfc "Файл $ORIGINAL_OUTPUT_DIR/dts/$dtb_name не существует\n\n" $ERROR
		return
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
		4) clear;return;;
		*) main;;
	esac
}

main
