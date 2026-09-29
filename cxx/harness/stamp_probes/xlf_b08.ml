module Simple = struct
  module type S = sig val v : int end
  module Register (D : S) = struct let x = D.v end
  module M = struct type t = int let v = 1 let w = 2 end
end
module X = Simple.Register (Simple.M)
