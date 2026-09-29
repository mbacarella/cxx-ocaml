module Foo = struct type t = int let x = 1 end
module type MT2 = sig type t val x : t end
module F (X : sig val v : (module MT2) end) = struct end
