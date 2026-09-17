let r = ref 0;;
let o = object val x = 33 method m = x end in
  let s = Marshal.to_string o [Marshal.Closures] in
  ignore s;;
