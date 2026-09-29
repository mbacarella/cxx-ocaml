module Foo = struct type t = int let x = 1 end
module type MT2 = sig type t val x : t end
module Bar = struct type t = int let x = 2 end
module T = (val
    (if true then (module Foo : MT2 with type t = int)
     else (module Bar : MT2 with type t = int))
    : MT2 with type t = int)
