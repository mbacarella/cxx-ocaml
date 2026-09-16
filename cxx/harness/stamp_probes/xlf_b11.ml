module Simple = struct
  module M = struct let v = 1 end
end
module F (Y : sig val v : int end) = struct let z = Y.v end
module X = F (Simple.M)
