module Foo = struct type t = int let x = 1 end
module type MT2 = sig type t val x : t end
let (p : (module MT2)) = (module Foo)
