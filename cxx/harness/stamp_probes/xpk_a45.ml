module Foo = struct type t = int let x = 1 end
module type MT2 = sig type t val x : t end
let g p = let (module X : MT2) = p in ()
