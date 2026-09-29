module Simple = struct
  module type S = sig val v : int end
  module Register (D : S) = struct let x = D.v end
  module M = struct let v = 1 module Q = struct let w = 2 end end
end
include Simple.M
