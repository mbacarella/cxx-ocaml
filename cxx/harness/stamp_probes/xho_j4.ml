module type S0 = sig type z1 type z2 end
module M0 = struct type z1 type z2 end
module type S0' = sig include S0 type additional end
module type S0f = sig type z1 type z2 type additional end
module F (X : S0) = struct type z1 type z2 type additional end
let f (x : (module S0')) = let module A = (val x) in ()
