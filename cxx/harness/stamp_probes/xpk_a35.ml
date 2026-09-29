module Foo = struct type t = int let x = 1 end
module type MT2 = sig type t val x : t end
module type S = sig val v : (module MT2) end
module M : S = struct let v = (module Foo : MT2) end
