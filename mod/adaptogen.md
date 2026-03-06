Mod Name: adaptogen

The purpose of this mod is to introduce a new PHP cast type named `(adapt)` that can trigger scheduled command execution through an external service and return a typed result directly in user code. The headline description is: "Add an Adaptogen cast operator that can adapt either a class reference or a constructed object into a live automation call and inferred PHP value." The mod name is `adaptogen`, and the intended language behavior is that both `(adapt) AdaptogenClass::class` and `(adapt) new AdaptogenClass($param1)` are valid native PHP syntax paths, so the feature feels like a core cast rather than a helper API.

At a high level, the cast accepts either a class-string target or an instantiated object target. When given a class reference, the runtime should resolve the class into an adaptogen-capable object before execution; when given `new AdaptogenClass($param1)`, it should preserve the explicit userland constructor arguments and also support autoloaded services in the constructor, similar to Symfony-style dependency injection, by resolving typed service dependencies automatically where possible. After the target object is ready, the runtime collects `cron()` and `command()` from that object, computes the cache key inside PHP, and sends `cron`, `command`, and `key` in a POST request to local port `245`. The hashing or final cache-key derivation must happen in PHP, not inside `./adaptogen/server.js`, so the service on port `245` should trust and use the Redis key supplied by PHP directly. The response is then interpreted into a PHP value in this fixed order: integer first (with `0` and `1` coerced to booleans), then float, then JSON decoded to an array, then case-insensitive `TRUE` or `FALSE` strings, and finally plain string fallback.

Caching behavior for instantiated objects must also be part of the runtime contract. If `(adapt)` is applied to constructed objects of the same class with different userland parameters, the cache key must distinguish those invocations so they do not collapse to the same Redis entry. In other words, `(adapt) new AdaptogenClass(1)` and `(adapt) new AdaptogenClass(2)` must not share a cached value; the effective cache identity should represent "AdaptogenClass with parameter 1" versus "AdaptogenClass with parameter 2". The preferred design in this mod is to let the class provide a `key()` method whose return value is derived from serialization of the passed-in constructor arguments that define the runtime identity, while autowired services remain infrastructure and do not need to pollute the cache key unless the class chooses to include them. PHP should then hash or finalize that key before sending it to port `245`, and the Node service should use the provided Redis key as-is instead of re-hashing the command.

Example usage should be part of the mod contract. A class-string form should continue to work like this:

```php
class AdaptogenClass
{
    public function cron(): string
    {
        return '* * * * *';
    }

    public function command(): string
    {
        return '/scripts/adaptogen.sh';
    }
}

$result = (adapt) AdaptogenClass::class;
```

The instantiated-object form must also work so userland parameters can be passed directly into the adaptogen object:

```php
class AdaptogenClass
{
    public function __construct(private int $tenantId)
    {
    }

    public function cron(): string
    {
        return '*/5 * * * *';
    }

    public function command(): string
    {
        return '/scripts/adaptogen.sh --tenant=' . $this->tenantId;
    }
}

$tenantId = 3;
$result = (adapt) new AdaptogenClass($tenantId);
```

Two instantiated adaptogen objects with different explicit parameters must resolve to different cache entries:

```php
$result1 = (adapt) new AdaptogenClass(1);
$result2 = (adapt) new AdaptogenClass(2);
```

In that scenario, `$result1` must not reuse the Redis value for `$result2`. The runtime should treat them as different adapted identities because the passed constructor arguments differ.

The autowired-service path must also be supported, so constructor dependencies can be resolved by type while explicit scalar or object arguments supplied by the caller remain intact:

```php
use Service1;
use Service2;

class AdaptogenClass
{
    public function __construct(
        private Service1 $service1,
        private Service2 $service2,
        private int $tenantId,
    ) {
    }

    public function cron(): string
    {
        return $this->service1->getCron();
    }

    public function command(): string
    {
        return $this->service2->getCommand($this->tenantId);
    }

    public function key(): string
    {
        return serialize($this->tenantId);
    }
}

$tenantId = 3;
$result = (adapt) new AdaptogenClass($tenantId);
```

If a `key()` method is present, the runtime should use it as the object-specific cache discriminator for the Redis entry associated with that adapted class invocation. This gives the class a native way to separate cached results for different constructor parameters while still allowing autowired services to be resolved automatically. The PHP runtime is responsible for turning that discriminator into the final Redis key sent over HTTP, and `./adaptogen/server.js` should consume that provided key directly.

This could work for three strong reasons. First, supporting both class references and instantiated objects gives developers a practical path for simple no-argument adaptogens and richer parameterized adaptogens without changing the `(adapt)` surface syntax. Second, constructor autowiring makes adaptogen classes fit naturally into modern PHP application structure, so `cron()` and `command()` can be assembled from real services instead of hard-coded static helpers. Third, centralizing execution through the existing port-245 service while keeping a fixed inference order preserves predictable runtime behavior and reduces repetitive parsing boilerplate in calling code.

This might not work for three practical reasons. First, adding a new cast type with support for both class names and constructed objects requires deeper parser, compiler, and VM work in Zend than a simpler class-string-only feature, especially if `new` expressions and object lifecycle rules must be preserved. Second, service autowiring inside the runtime creates container-resolution questions around scope, configuration, missing services, and framework interoperability that are much harder than calling static methods on a known class. Third, the feature still depends on a local HTTP service and heuristic response typing, so runtime outages, ambiguous output, or mismatches between supplied constructor arguments and autoloaded services could produce confusing failures unless the implementation is strict and well-defined.
