<?php

class AdaptogenClass {

   private int $param;

   public function __construct(int $param) {
      $this->param = $param;
   }

   public function cron(): string {

      // every "$this->param" minutes
      return sprintf('*/%d * * * *', $this->param);
   }

   public function command(): string {

      // params passed into __construct available here
      return '/scripts/adaptogen.sh';
   }

   public function key(): string {

      // cache key
      return (string) $this->param;
   }

}

// these are different adapted objects, so they produce different PHP-side cache keys
// arbitrary arguments passed into AdaptogenClass, could be anything
// in this case, the cron function uses it to determine interval length
$result1 = (adapt) new AdaptogenClass(1);
$result2 = (adapt) new AdaptogenClass(2);

echo $result1 . "\n";
echo $result2 . "\n";

// before script change:
// working
// working

// after script change after one minute:
// working2
// working

// after script change after two minutes:
// working2
// working2