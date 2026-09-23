module F (X : sig end) (Y : sig end) = struct let n = 1 end
module B = F (struct end) (struct end)
