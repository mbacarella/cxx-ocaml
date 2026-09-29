module F (X : sig end) = struct let n = 1 end
module B = F (struct end)
module C = F (struct end)
