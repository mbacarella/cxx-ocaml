module M : sig val f : float array -> float array end =
struct let f a = a end
let v = M.f [||]
