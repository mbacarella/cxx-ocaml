module M = struct type e = A type f = { x : e } end
let j (r : M.f) = r.x
