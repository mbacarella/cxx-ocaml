module Simple = struct
  module type S = sig val v : int end
  module Register (D : S) = struct let x = D.v type t = int end
end
module N = struct let v = 1 end
module X = Simple.Register (N)
