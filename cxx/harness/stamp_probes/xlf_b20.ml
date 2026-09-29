module Simple = struct
  module type S = sig val v : int end
  module type T = sig val x : int end
  module Register (D : S) : T = struct let x = D.v end
end
module X = Simple.Register (struct let v = 1 end)
