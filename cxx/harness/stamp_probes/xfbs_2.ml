module F (X: sig type t end) = struct type u = X.t option module N =
  struct type w = int let z : w = 0 let y : u = None let x : X.t list =
  [] end end module A = struct type t = bool end module C = F(A) let z =
  C.N.z let y = C.N.y let x = C.N.x
