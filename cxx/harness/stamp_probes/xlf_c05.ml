module Simple = struct
  module type S = sig val v : int end
  module Register (D : S) : sig val x : int module Q : sig val w : int end
  end =
    struct let x = D.v module Q = struct let w = 2 end end
end
module N = struct let v = 1 end
module X = Simple.Register (N)
