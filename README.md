# Why Is There No Invisible Database?
## The Idea
In this post I'll write about the exploration of an old idea of mine, which led almost nowhere, so it might not be the most informative piece. If you have time to spare, go ahead. This will also serve as a reminder to myself when I revisit the idea in a few years.

For a few years I have been thinking about a database that requires little to no explicit storage and loading. I imagined simply marking a `std::vector` for example as "in the database" and it would all just magically happen. I wouldn't have to create wrapper classes and or write tedious serialization/deserialization code. Most developery dislike writing plumbing code, right? I wanted a system that automatically retrieves the newest version whenever I access a data structure and sends a modified version back to the server, when I modify it. And ideally it would work with arbitrary data structures, without any extra work. Something like this:

```cpp
struct TestStruct {
    std::string str;
    unsigned int num;
};

// You need some sort of key, so you can identify that vectors across different machines belong together.
InTheDatabase<std::vector<TestStruct>> vec("vec");

vec->push_back(TestStruct { "Joel", 69 /*nice*/ });
// "Joel" is now in "vec" and available on every connected client machine

// This might print a bunch of `str`s, that we did not put into "vec", but another client might have put there
for (const auto& elem : *vec) {
    fmt::print("{}: {}\n", elem.str, elem.num);
}
```

## The Missing Piece

Intuitively I thought that `InTheDatabase` would somehow inject a custom allocator that would track all dynamic allocations the type might be doing and keep track of that memory.
It seemed to me the biggest problem would be detecting all modifications done to these tracked memory ranges. And because there was no way to do something like that (there is), I never really spent much time on the idea.

Then, a few days ago, I learned about the "`PROT_NONE/SIGSEGV` trick".
It involves using [mprotect](https://man7.org/linux/man-pages/man2/mprotect.2.html) to change the access protections of a range of memory to `PROT_NONE` (for this use case `PROT_READ` would work too), which disallows any accesses and installing a signal handler for SIGSEGV, which would be triggered whenever the protected region would be accessed.
You could then detect these memory accesses and note down the page that was accessed and restore the access protections to whatever you had before (probably `PROT_READ | PROT_WRITE`).
This was my missing piece (*so I thought*)! Maybe I could use this to implement my invisible database?!
Shortly (seconds actually) after that I learned, that there is also [userfaultfd](https://www.kernel.org/doc/html/latest/admin-guide/mm/userfaultfd.html), which is a Linux feature that is dedicated to detecting page-faults in user space in a clean way, which can do this as well in a "proper and more optimal" way.

## A Revised Sketch

This idea made me engage with this invisible database more seriously. The first thing I realized is that in order to maintain invariants of the classes being stored in the database, we have to introduce transactions. If we simply synchronize every modified memory region as soon as we can, we can easily bring data structures into invalid states. Data structures may also span multiple pages (esp. if they do many allocations) or cross page boundaries. For example if we `push_back`-ed an element into a vector, we might update the size first and then actually allocate the new element (which might be on a different page). Of course it could be the other way around as well, which would be better for us, but we should not rely on implementation details we cannot control (or sometimes even know) to maintain valid data structures. Very similar to multi-threaded programming, you have to protect the integrity (invariants) of these data structures with some sort of mutex. This invites the introduction of another abstraction of a memory region in which we can place as many data structures as we want and that we can lock and unlock to modify it transactionally:

```cpp
// the last argument is region size in pages
invisibledb::SyncedMemoryRegion region(server, "region", 4);

struct TestStruct {
    std::pmr::string str;
    unsigned int num;
};

// This will lock the region on the server preventing any other client from locking it for a while,
// and read the current version from the server.
// It might also actually lock a mutex, so you can use the SyncedMemoryRegion from multiple threads.
region.lock();
const auto vec = region.getObject<std::pmr::vector<TestStruct>>("vec");
// Here we will send all modified pages to the server and the region will be unlocked.
region.unlock();

// ...

region.lock()
vec->push_back(TestStruct { "Joel", 69 /*nice*/ });

for (const auto& elem : *vec) {
    fmt::print("{}: {}\n", elem.str, elem.num);
}
region.unlock();
```

There are also some other small changes in this snippet. To inject the allocator, we used the standard library types from the `std::pmr` (polymorphic memory resource) namespace. Also of course we need some object that represents the database server, `server`, which we need to pass into the `SyncedMemoryRegion`. Where it comes from, how we connect and anything like that is not important for our sketch. The `getObject` factory function either creates the vector on the heap, inside the memory region or returns the instance, if an object with that name is already in the region. It was already like that in the first sketch, but it should be mentioned that the vector itself must not live on the stack, or it would not be synchronized properly (duh).

Additionally because we can track memory accesses already, we can do cool things like asserting when the memory is accessed without the region being locked, because sometimes that's the only way they're gonna learn.

Note that it's very easy to be less than extremely careful here and use the wrong allocator by accident. For example like this:

```cpp
std::pmr::string str = "Joel";
vec->push_back(TestStruct { std::move(str), 69 });
```

Here the `str` field in our `TestStruct` object is **not** allocated in the memory region managed by the database!
That's a very easy mistake, hard to catch and the behavior exhibited will be maddeningly weird. Bad sign.

Also we need to consider that the allocator for the `SyncedMemoryRegion` will have to keep it's state inside the memory region as well. That means all variables that control the allocator behavior have to be inside this memory region and have to be synchronized. Otherwise we might allocate from the start of the region multiple times on different clients and overwrite things by accident. For that reason alone we have to roll our own allocator (or multiple, depending on use case). But there is also another, smaller reason, which is that we require perfectly predictable allocator behavior. This almost gives us no other choice but to roll our own. Imagine that on one client an allocator might reuse a previously deallocated region and on another client a different allocator (maybe from a different standard library implementation) might reuse another. A more practical reason is also, is that the pmr containers keep a pointer to the allocator, which of course should not point outside the synchronized memory region as well.

And thinking about this it also becomes clear that not just all the allocators have to have the same behavior, but all the data structures involved to. So to be sure, you probably want to use **exactly** the same standard library version for each client. A small corollary, which might be even more obvious, is that we cannot mix little and big endian clients. That might be a bit too restrictive to be useful.

## The Prototype

This is about where I started implementing this thing. If you were smarter than me, you might have figured this out before starting, but pretty much all the interesting data structures have pointers. And even though they will point into the synchronized memory region, if you do everything right, those addresses might (very likely) not be correct on other clients, because the base address of the memory region will be different on different clients. I could not figure out how to fix this properly, except by walking through the whole modified memory region and finding everything that is aligned like a pointer and contains a value that looks like a pointer (i.e. an address inside of this memory region). After finding those addresses, I make them relative, save their offsets and restore them with the proper base address on reception. Yes, this could give false positives, which should be unlikely, but would lead to the most confusing bugs.

You can check out my proof-of-concept implementation here: https://github.com/pfirsich/invisibledb

In my implementation I took some shortcuts for the first (and likely last) prototype. The `Server` object does not actually connect anywhere and `Server::sendPage` and `Server::receivePage` simply `memcpy` some memory to and from a buffer. Similarly `Server::lockRegion` and `Server::unlockRegion` simply set a boolean flag. The `TrackedMemoryRegion` class, which was supposed to do the cool stuff - tracking writes using `usefaultfd` - currently only uses `memcmp` to check whether a page was modified. I wanted to keep the complexity low whereever possible until I implemented the tricky bits. That turned out to be wise, because, as I have explained above, they were very tricky.

Also the allocator is a very simple linear allocator that can not reuse freed memory and simply increases an offset from which it gives out memory. In a real world application you definitely want something smarter and likely different allocators for different use cases and data structures. You would possibly need to support multiple different allocators in the same region.

Of course there are a couple more ways to improve this, which are not really relevant at the level of seriousness of this project.

## Conclusion

Now that I have finally implemented a (not so) invisible database, I know why it didn't exist before or at least why it's not very popular. It was tricky to build and a fun exercise, but there are some downsides to using it, which are the answer to "Why is there no invisible database?":

* Visiblity: The resulting system is not nearly as invisible as we would like it to be (explicit locking, specific ways of creating objects, ...).
* Usability (arguably still "visibility"): It's extremely easy to misuse and the resulting bugs are very confusing. Having a variable on the stack, forgetting to inject the right allocator or forgetting to lock the region, etc. can cause significant headaches.
* Performance: While you do save the serialization/deserializion logic there is significant overhead. Intercepting every memory access is likely inefficient. As is the scanning of memory for pointer-like values during synchronization. There are probably some access patterns where it might be faster than any other database, but I cannot come up with them and I definitely did not prove it.
* Portability: You cannot mix clients with different endianness or even standard library versions and maybe even compilers. And, of course, `userfaultfd` is currently limited to Linux only! You can only use it with C or C++ and those languages are not considered very cool or 🚀 blazingly fast 🚀 anymore. Even if you use exactly the same software on every client, you probably want to upgrade at some point. Since you are storing memory regions, an ABI break in some library you use might break your weekend.
* Complexity: Despite ignoring many complications (a real server, proper allocators, proper write tracking), the resulting piece of software is quite the monstrosity and I am sure you can measure my heart rate increase, when I look at it. Introducing complexity usually implies future costs.

And even if any of these are no concern to you, you don't really know if it will stay like that forever. I would not feel comfortable introducing something like this into my project. Even if I had made it!

If we finally (ever?) get reflection in C++, another approach might to generate serialization/deserialization logic for [POD](https://en.wikipedia.org/wiki/Passive_data_structure)s and provide some wrappers for standard library classes to simply interface with any of the popular databases. This would essentially be just a fancy client library for an existing database. And if we ever do get reflection, I will probably try that.

Expectedly my idea did not lead to a groundbreaking conclusion, but I found it very worthwile to explore this topic more deeply and the better I understand the remaining problems, the more likely I am to find solutions for them in the future.
