module Foo = struct type t = int let x = 1 end
module type MT2 = sig type t val x : t end
module M = struct let q = (module Foo : MT2) end
