module Simple = struct
  module type S = sig val v : int end
  module Register (D : S) = struct let x = D.v end
end
module N = struct let v = 1 end
let r = let module X = Simple.Register (N) in X.x
