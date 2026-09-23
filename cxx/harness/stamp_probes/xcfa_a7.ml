let f x = let tmp = x + 1 in tmp
module F (X : sig end) = struct end
module B = F (struct end)
type after = A | B
