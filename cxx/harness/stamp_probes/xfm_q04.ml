let r = ref 0;;
let o = object val x = 33 method m = x end in
  let s = Marshal.to_string o [Marshal.Closures] in
  let o' : <m:int> = Marshal.from_string s 0 in
  let o'' : <m:int> = Marshal.from_string s 0 in
  assert ((o#m, o'#m) = (33, 33));;
