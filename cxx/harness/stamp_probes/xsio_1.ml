module type S0 = sig type key val k : int end
module M : sig include S0 val z : int end =
  struct type key let k = 1 let z = 2 end
