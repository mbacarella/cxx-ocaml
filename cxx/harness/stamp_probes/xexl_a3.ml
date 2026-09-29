module type S = sig
  exception E of {lbl : int}
end
