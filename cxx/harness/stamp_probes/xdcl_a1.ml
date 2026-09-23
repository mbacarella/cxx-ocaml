module F (X : sig end) = struct let n = 1 end
module B = F (struct end)
let u = B.n
