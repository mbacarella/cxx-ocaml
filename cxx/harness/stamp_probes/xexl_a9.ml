exception E of {x : int}
module N = struct
  exception E2 = E
end
include N
