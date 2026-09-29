module Foo = struct type t = int let x = 1 end
module type MT2 = sig type t val x : t end
let f (x : (module MT2)) = ()
let p = (module Foo : MT2)
let _ = f p
