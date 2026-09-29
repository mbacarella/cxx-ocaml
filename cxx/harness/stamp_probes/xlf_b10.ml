module Simple = struct
  module type S = sig val v : int end
  module Inner = struct
    module Register (D : S) = struct let x = D.v end
    let q = 1
  end
end
module N = struct let v = 1 end
module X = Simple.Inner.Register (N)
