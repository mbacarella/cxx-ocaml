module Simple = struct
  module Register (D : sig val v : int end) = struct let x = D.v end
end
module N = struct let v = 1 end
module X = Simple.Register (N)
