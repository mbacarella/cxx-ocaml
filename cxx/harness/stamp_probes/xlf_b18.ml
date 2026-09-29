module Simple = struct
  module type S = sig val v : int end
  module Register (D : S) = struct let x = D.v end
  module M = struct let v = 1 end
  module N = struct let v = 2 end
end
module X = Simple.Register (Simple.M)
module Y = Simple.Register (Simple.N)
