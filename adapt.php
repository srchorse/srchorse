<?php

class AdaptogenClass {

	public static function cron(): string {
		return '* * * * *';
	}

	public static function command(): string {
		return '/scripts/adaptogen.sh';
	}

}

$result = (adapt) AdaptogenClass::class;

echo $result;

?>
