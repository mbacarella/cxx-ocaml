module Simple = struct
  module Register (D : sig val v : int module Q : sig val w : int end end) =
    struct let x = D.v end
end
module N = struct let v = 1 module Q = struct let w = 2 end end
module X = Simple.Register (N)
