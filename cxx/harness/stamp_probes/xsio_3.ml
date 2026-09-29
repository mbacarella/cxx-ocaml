module type S0 = sig val k : int end
module A = struct
  module B : sig include S0 val z : int end = struct let k = 1 let z = 2 end
end
