module F (X: sig type t end) = struct module N = struct type w = X.t let z
  : w list = [] end end module A = struct type t = bool end module C = F(A)
  let z = C.N.z
