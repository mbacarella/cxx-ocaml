module Simple = struct
  module type S = sig val v : int end
  module Register (D : S) = struct let x = D.v end
  module Inner = struct module M = struct let v = 1 end let q = 1 end
end
module X = Simple.Register (Simple.Inner.M)
