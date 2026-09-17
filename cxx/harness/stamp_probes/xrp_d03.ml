module type A = sig type t = private int end
module F (X : A) = struct include X let y = 1 let z = 1 end
