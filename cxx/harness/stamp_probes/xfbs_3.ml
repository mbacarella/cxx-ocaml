module F (X: sig type t end) = struct module N = struct type w = A of X.t
  let z x = A x end end module A = struct type t = bool end module C = F(A)
  let z = C.N.z
