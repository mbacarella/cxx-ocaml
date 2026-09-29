let f x = let tmp = x + 1 in tmp
module F (X : sig end) (Y : sig end) = struct end
module B = F (struct end) (struct end)
type after = A | B
