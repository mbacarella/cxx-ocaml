module Simple : sig
  module type S = sig val v : int end
  module Register (D : S) : sig val x : int end
  module M : S
end = struct
  module type S = sig val v : int end
  module Register (D : S) = struct let x = D.v let y = 2 end
  module M = struct let v = 1 end
end
module X = Simple.Register (Simple.M)
